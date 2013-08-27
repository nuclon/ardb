 /*
 *Copyright (c) 2013-2013, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 *
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * rdb.cpp
 *
 *  Created on: 2013年8月19日
 *      Author: yinqiwen
 */
#include "rdb.hpp"

#include "util/helpers.hpp"

extern "C"
{
#include "redis/lzf.h"
#include "redis/ziplist.h"
#include "redis/zipmap.h"
#include "redis/intset.h"
#include "redis/crc64.h"
#include "redis/endianconv.h"
}

#define REDIS_RDB_VERSION 6

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 01, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the REDIS_RDB_ENC_* defines.
 *
 * Lengths up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define REDIS_RDB_ENC_INT8 0        /* 8 bit signed integer */
#define REDIS_RDB_ENC_INT16 1       /* 16 bit signed integer */
#define REDIS_RDB_ENC_INT32 2       /* 32 bit signed integer */
#define REDIS_RDB_ENC_LZF 3         /* string compressed with FASTLZ */

/* Special RDB opcodes (saved/loaded with rdbSaveType/rdbLoadType). */
#define REDIS_RDB_OPCODE_EXPIRETIME_MS 252
#define REDIS_RDB_OPCODE_EXPIRETIME 253
#define REDIS_RDB_OPCODE_SELECTDB   254
#define REDIS_RDB_OPCODE_EOF        255

/* Dup object types to RDB object types. Only reason is readability (are we
 * dealing with RDB types or with in-memory object types?). */
#define REDIS_RDB_TYPE_STRING 0
#define REDIS_RDB_TYPE_LIST   1
#define REDIS_RDB_TYPE_SET    2
#define REDIS_RDB_TYPE_ZSET   3
#define REDIS_RDB_TYPE_HASH   4

/* Object types for encoded objects. */
#define REDIS_RDB_TYPE_HASH_ZIPMAP    9
#define REDIS_RDB_TYPE_LIST_ZIPLIST  10
#define REDIS_RDB_TYPE_SET_INTSET    11
#define REDIS_RDB_TYPE_ZSET_ZIPLIST  12
#define REDIS_RDB_TYPE_HASH_ZIPLIST  13

namespace ardb
{
	RedisDumpFile::RedisDumpFile(Ardb* db, const std::string& file) :
			m_read_fp(NULL), m_write_fp(NULL), m_file_path(file), m_current_db(
					0), m_db(db), m_cksm(0), m_load_cb(NULL), m_load_cbdata(
			NULL)
	{
	}

	void RedisDumpFile::Close()
	{
		if (NULL != m_read_fp)
		{
			fclose(m_read_fp);
			m_read_fp = NULL;
		}
		if (NULL != m_write_fp)
		{
			fclose(m_write_fp);
			m_read_fp = NULL;
		}
	}

	int RedisDumpFile::ReadType()
	{
		unsigned char type;
		if (Read(&type, 1) == 0)
			return -1;
		return type;
	}

	time_t RedisDumpFile::ReadTime()
	{
		int32_t t32;
		if (Read(&t32, 4) == 0)
			return -1;
		return (time_t) t32;
	}
	int64 RedisDumpFile::ReadMillisecondTime()
	{
		int64_t t64;
		if (Read(&t64, 8) == 0)
			return -1;
		return (long long) t64;
	}

	uint32_t RedisDumpFile::ReadLen(int *isencoded)
	{
		unsigned char buf[2];
		uint32_t len;
		int type;

		if (isencoded)
			*isencoded = 0;
		if (Read(buf, 1) == 0)
			return REDIS_RDB_LENERR;
		type = (buf[0] & 0xC0) >> 6;
		if (type == REDIS_RDB_ENCVAL)
		{
			/* Read a 6 bit encoding type. */
			if (isencoded)
				*isencoded = 1;
			return buf[0] & 0x3F;
		} else if (type == REDIS_RDB_6BITLEN)
		{
			/* Read a 6 bit len. */
			return buf[0] & 0x3F;
		} else if (type == REDIS_RDB_14BITLEN)
		{
			/* Read a 14 bit len. */
			if (Read(buf + 1, 1) == 0)
				return REDIS_RDB_LENERR;
			return ((buf[0] & 0x3F) << 8) | buf[1];
		} else
		{
			/* Read a 32 bit len. */
			if (Read(&len, 4) == 0)
				return REDIS_RDB_LENERR;
			return ntohl(len);
		}
	}

	bool RedisDumpFile::ReadLzfStringObject(std::string& str)
	{
		unsigned int len, clen;
		unsigned char *c = NULL;

		if ((clen = ReadLen(NULL)) == REDIS_RDB_LENERR)
			return NULL;
		if ((len = ReadLen(NULL)) == REDIS_RDB_LENERR)
			return NULL;
		str.resize(len);
		NEW(c, unsigned char[clen]);
		if (NULL == c)
			return false;

		if (!Read(c, clen))
		{
			DELETE_A(c);
			return false;
		}
		str.resize(len);
		char* val = &str[0];
		if (lzf_decompress(c, clen, val, len) == 0)
		{
			DELETE_A(c);
			return false;
		}
		DELETE_A(c);
		return true;
	}

	bool RedisDumpFile::ReadInteger(int enctype, int64& val)
	{
		unsigned char enc[4];

		if (enctype == REDIS_RDB_ENC_INT8)
		{
			if (Read(enc, 1) == 0)
				return false;
			val = (signed char) enc[0];
		} else if (enctype == REDIS_RDB_ENC_INT16)
		{
			uint16_t v;
			if (Read(enc, 2) == 0)
				return false;
			v = enc[0] | (enc[1] << 8);
			val = (int16_t) v;
		} else if (enctype == REDIS_RDB_ENC_INT32)
		{
			uint32_t v;
			if (Read(enc, 4) == 0)
				return false;
			v = enc[0] | (enc[1] << 8) | (enc[2] << 16) | (enc[3] << 24);
			val = (int32_t) v;
		} else
		{
			val = 0; /* anti-warning */
			FATAL_LOG("Unknown RDB integer encoding type");
			abort();
		}
		return true;
	}

	/* For information about double serialization check rdbSaveDoubleValue() */
	bool RedisDumpFile::ReadDoubleValue(double&val)
	{
		static double R_Zero = 0.0;
		static double R_PosInf = 1.0 / R_Zero;
		static double R_NegInf = -1.0 / R_Zero;
		static double R_Nan = R_Zero / R_Zero;
		char buf[128];
		unsigned char len;

		if (Read(&len, 1) == 0)
			return -1;
		switch (len)
		{
			case 255:
				val = R_NegInf;
				return 0;
			case 254:
				val = R_PosInf;
				return 0;
			case 253:
				val = R_Nan;
				return 0;
			default:
				if (Read(buf, len) == 0)
					return -1;
				buf[len] = '\0';
				sscanf(buf, "%lg", &val);
				return 0;
		}
	}

	bool RedisDumpFile::ReadString(std::string& str)
	{
		str.clear();
		int isencoded;
		uint32_t len;
		len = ReadLen(&isencoded);
		if (isencoded)
		{
			switch (len)
			{
				case REDIS_RDB_ENC_INT8:
				case REDIS_RDB_ENC_INT16:
				case REDIS_RDB_ENC_INT32:
				{
					int64 v;
					if (ReadInteger(len, v))
					{
						str = stringfromll(v);
						return true;
					} else
					{
						return true;
					}
				}
				case REDIS_RDB_ENC_LZF:
				{
					return ReadLzfStringObject(str);
				}
				default:
				{
					ERROR_LOG("Unknown RDB encoding type");
					abort();
					break;
				}
			}
		}

		if (len == REDIS_RDB_LENERR)
			return false;
		str.clear();
		str.resize(len);
		char* val = &str[0];
		if (len && Read(val, len) == 0)
		{
			return false;
		}
		return true;
	}

	void RedisDumpFile::LoadListZipList(unsigned char* data,
			const std::string& key)
	{
		unsigned char* iter = ziplistIndex(data, 0);
		while (iter != NULL)
		{
			unsigned char *vstr;
			unsigned int vlen;
			long long vlong;
			if (ziplistGet(iter, &vstr, &vlen, &vlong))
			{
				Slice value;
				if (vstr)
				{
					value = Slice((char*) vstr, vlen);
				} else
				{
					value = stringfromll(vlong);
				}
				m_db->LPush(m_current_db, key, value);
			}
			iter = ziplistNext(data, iter);
		}
	}

	static double zzlGetScore(unsigned char *sptr)
	{
		unsigned char *vstr;
		unsigned int vlen;
		long long vlong;
		char buf[128];
		double score;

		ASSERT(sptr != NULL);
		ASSERT(ziplistGet(sptr, &vstr, &vlen, &vlong));
		if (vstr)
		{
			memcpy(buf, vstr, vlen);
			buf[vlen] = '\0';
			score = strtod(buf, NULL);
		} else
		{
			score = vlong;
		}
		return score;
	}
	void RedisDumpFile::LoadZSetZipList(unsigned char* data,
			const std::string& key)
	{
		unsigned char* iter = ziplistIndex(data, 0);
		while (iter != NULL)
		{
			unsigned char *vstr;
			unsigned int vlen;
			long long vlong;
			Slice value;
			if (ziplistGet(iter, &vstr, &vlen, &vlong))
			{
				if (vstr)
				{
					value = Slice((char*) vstr, vlen);
				} else
				{
					value = stringfromll(vlong);
				}
			}
			iter = ziplistNext(data, iter);
			if (NULL == iter)
			{
				break;
			}
			double score = zzlGetScore(iter);
			m_db->ZAdd(m_current_db, key, score, value);
		}
	}

	void RedisDumpFile::LoadHashZipList(unsigned char* data,
			const std::string& key)
	{
		unsigned char* iter = ziplistIndex(data, 0);
		while (iter != NULL)
		{
			unsigned char *vstr, *fstr;
			unsigned int vlen, flen;
			long long vlong, flong;
			Slice field, value;
			if (ziplistGet(iter, &fstr, &flen, &flong))
			{
				if (fstr)
				{
					value = Slice((char*) fstr, flen);
				} else
				{
					value = stringfromll(flong);
				}
				m_db->LPush(m_current_db, key, value);
			} else
			{
				break;
			}
			iter = ziplistNext(data, iter);
			if (NULL == iter)
			{
				break;
			}
			if (ziplistGet(iter, &vstr, &vlen, &vlong))
			{
				if (vstr)
				{
					value = Slice((char*) vstr, vlen);
				} else
				{
					value = stringfromll(vlong);
				}
				m_db->HSet(m_current_db, key, field, value);
			}
			iter = ziplistNext(data, iter);
		}
	}

	void RedisDumpFile::LoadSetIntSet(unsigned char* data,
			const std::string& key)
	{
		int ii = 0;
		int64_t llele = 0;
		while (!intsetGet((intset*) data, ii++, &llele))
		{
			//value
			m_db->SAdd(m_current_db, key, stringfromll(llele));
		}
	}

	bool RedisDumpFile::LoadObject(int rdbtype, const std::string& key)
	{
		switch (rdbtype)
		{
			case REDIS_RDB_TYPE_STRING:
			{
				std::string str;
				if (ReadString(str))
				{
					//save key-value
					m_db->Set(m_current_db, key, str);
				} else
				{
					return false;
				}
				break;
			}
			case REDIS_RDB_TYPE_LIST:
			case REDIS_RDB_TYPE_SET:
			{
				uint32 len;
				if ((len = ReadLen(NULL)) == REDIS_RDB_LENERR)
					return false;
				while (len--)
				{
					std::string str;
					if (ReadString(str))
					{
						//push to list/set
						if (REDIS_RDB_TYPE_SET == rdbtype)
						{
							m_db->SAdd(m_current_db, key, str);
						} else
						{
							m_db->LPush(m_current_db, key, str);
						}
					} else
					{
						return false;
					}
				}
				break;
			}
			case REDIS_RDB_TYPE_ZSET:
			{
				uint32 len;
				if ((len = ReadLen(NULL)) == REDIS_RDB_LENERR)
					return false;
				while (len--)
				{
					std::string str;
					double score;
					if (ReadString(str) && ReadDoubleValue(score))
					{
						//save value score
						m_db->ZAdd(m_current_db, key, score, str);
					} else
					{
						return false;
					}
				}
				break;
			}
			case REDIS_RDB_TYPE_HASH:
			{
				uint32 len;
				if ((len = ReadLen(NULL)) == REDIS_RDB_LENERR)
					return false;
				while (len--)
				{
					std::string field, str;
					if (ReadString(field) && ReadString(str))
					{
						//save hash value
						m_db->HSet(m_current_db, key, field, str);
					} else
					{
						return false;
					}
				}
				break;
			}
			case REDIS_RDB_TYPE_HASH_ZIPMAP:
			case REDIS_RDB_TYPE_LIST_ZIPLIST:
			case REDIS_RDB_TYPE_SET_INTSET:
			case REDIS_RDB_TYPE_ZSET_ZIPLIST:
			case REDIS_RDB_TYPE_HASH_ZIPLIST:
			{
				std::string aux;
				if (!ReadString(aux))
				{
					return false;
				}
				unsigned char* data = (unsigned char*) (&(aux[0]));
				switch (rdbtype)
				{
					case REDIS_RDB_TYPE_HASH_ZIPMAP:
					{
						unsigned char *zi = zipmapRewind(data);
						unsigned char *fstr, *vstr;
						unsigned int flen, vlen;
						unsigned int maxlen = 0;
						while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen))
								!= NULL)
						{
							if (flen > maxlen)
								maxlen = flen;
							if (vlen > maxlen)
								maxlen = vlen;

							//save hash value data
							Slice field((char*) fstr, flen);
							Slice value((char*) vstr, vlen);
							m_db->HSet(m_current_db, key, field, value);
						}
						break;
					}
					case REDIS_RDB_TYPE_LIST_ZIPLIST:
					{
						LoadListZipList(data, key);
						break;
					}
					case REDIS_RDB_TYPE_SET_INTSET:
					{
						LoadSetIntSet(data, key);
						break;
					}
					case REDIS_RDB_TYPE_ZSET_ZIPLIST:
					{
						LoadZSetZipList(data, key);
						break;
					}
					case REDIS_RDB_TYPE_HASH_ZIPLIST:
					{
						LoadHashZipList(data, key);
						break;
					}
					default:
					{
						ERROR_LOG("Unknown encoding");
						abort();
					}
				}
				break;
			}
			default:
			{
				ERROR_LOG("Unknown object type");
				abort();
			}
		}
		return true;
	}

	bool RedisDumpFile::Read(void* buf, size_t buflen, bool cksm)
	{
		static uint64 routinetime = get_current_epoch_millis();
		/*
		 * routine callback every 100ms
		 */
		if (NULL != m_load_cb
				&& get_current_epoch_millis() - routinetime >= 100)
		{
			m_load_cb(m_load_cbdata);
			routinetime = get_current_epoch_millis();
		}
		size_t max_read_bytes = 1024 * 1024 * 2;
		while (buflen)
		{
			size_t bytes_to_read =
					(max_read_bytes < buflen) ? max_read_bytes : buflen;
			if (fread(buf, bytes_to_read, 1, m_read_fp) == 0)
				return false;
			if (cksm)
			{
				//check sum here
				m_cksm = crc64(m_cksm, (unsigned char *) buf, bytes_to_read);
			}
			buf = (char*) buf + bytes_to_read;
			buflen -= bytes_to_read;
		}
		return true;
	}

	int RedisDumpFile::Load(LoadRoutine* cb, void *data)
	{
		m_load_cb = cb;
		m_load_cbdata = data;
		Close();
		if ((m_read_fp = fopen(m_file_path.c_str(), "r")) == NULL)
		{
			ERROR_LOG("Failed to load redis dump file:%s", m_file_path.c_str());
			return -1;
		}
		char buf[1024];
		int rdbver, type;
		int64 expiretime = -1;
		std::string key;

		m_current_db = 0;
		if (!Read(buf, 9, true))
			goto eoferr;
		buf[9] = '\0';
		if (memcmp(buf, "REDIS", 5) != 0)
		{
			Close();
			WARN_LOG("Wrong signature trying to load DB from file");
			return -1;
		}
		rdbver = atoi(buf + 5);
		if (rdbver < 1 || rdbver > REDIS_RDB_VERSION)
		{
			WARN_LOG("Can't handle RDB format version %d", rdbver);
			return -1;
		}

		while (true)
		{
			expiretime = -1;
			/* Read type. */
			if ((type = ReadType()) == -1)
				goto eoferr;
			if (type == REDIS_RDB_OPCODE_EXPIRETIME)
			{
				if ((expiretime = ReadTime()) == -1)
					goto eoferr;
				/* We read the time so we need to read the object type again. */
				if ((type = ReadType()) == -1)
					goto eoferr;
				/* the EXPIRETIME opcode specifies time in seconds, so convert
				 * into milliseconds. */
				expiretime *= 1000;
			} else if (type == REDIS_RDB_OPCODE_EXPIRETIME_MS)
			{
				/* Milliseconds precision expire times introduced with RDB
				 * version 3. */
				if ((expiretime = ReadMillisecondTime()) == -1)
					goto eoferr;
				/* We read the time so we need to read the object type again. */
				if ((type = ReadType()) == -1)
					goto eoferr;
			}

			if (type == REDIS_RDB_OPCODE_EOF)
				break;

			/* Handle SELECT DB opcode as a special case */
			if (type == REDIS_RDB_OPCODE_SELECTDB)
			{
				if ((m_current_db = ReadLen(NULL)) == REDIS_RDB_LENERR)
				{
					ERROR_LOG("Failed to read current DBID.");
					goto eoferr;
				}
				continue;
			}
			//load key, object

			if (!ReadString(key))
			{
				ERROR_LOG("Failed to read current key.");
				goto eoferr;
			}
			if (!LoadObject(type, key))
			{
				ERROR_LOG("Failed to load object:%d", type);
				goto eoferr;
			}
			if (-1 != expiretime)
			{
				m_db->Pexpireat(m_current_db, key, expiretime);
			}
		}

		/* Verify the checksum if RDB version is >= 5 */
		if (rdbver >= 5)
		{
			uint64_t cksum, expected = m_cksm;
			if (!Read(&cksum, 8))
			{
				goto eoferr;
			}
			memrev64ifbe(&cksum);
			if (cksum == 0)
			{
				WARN_LOG(
						"RDB file was saved with checksum disabled: no check performed.");
			} else if (cksum != expected)
			{
				ERROR_LOG("Wrong RDB checksum. Aborting now(%llu-%llu)", cksum, expected);
				exit(1);
			}
		}
		Close();
		INFO_LOG("Redis dump file load finished.");
		return 0;
		eoferr: Close();
		WARN_LOG(
				"Short read or OOM loading DB. Unrecoverable error, aborting now.");
		return -1;
	}

	int RedisDumpFile::Write(const char* buf, size_t buflen)
	{
		if (NULL == m_write_fp)
		{
			if ((m_write_fp = fopen(m_file_path.c_str(), "w")) == NULL)
			{
				ERROR_LOG("Failed to open redis dump file:%s to write",
						m_file_path.c_str());
				return -1;
			}
		}

		size_t max_write_bytes = 1024 * 1024 * 2;
		const char* data = buf;
		while (buflen)
		{
			size_t bytes_to_write =
					(max_write_bytes < buflen) ? max_write_bytes : buflen;
			if (fwrite(data, bytes_to_write, 1, m_write_fp) == 0)
				return -1;
			data += bytes_to_write;
			buflen -= bytes_to_write;
		}
		return 0;
	}

	void RedisDumpFile::Flush()
	{
		if (NULL != m_write_fp)
		{
			fflush(m_write_fp);
		}
	}

	RedisDumpFile::~RedisDumpFile()
	{
		Close();
	}
}
