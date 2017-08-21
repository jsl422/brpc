// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2015 Baidu.com, Inc. All Rights Reserved

// Author: Ge,Jun (gejun@baidu.com)
// Date: Fri Jun  5 18:25:40 CST 2015

#include <limits>
#include "base/logging.h"
#include "brpc/redis_reply.h"


namespace brpc {

//BAIDU_CASSERT(sizeof(RedisReply) == 24, size_match);

const char* RedisReplyTypeToString(RedisReplyType type) {
    switch (type) {
    case REDIS_REPLY_STRING: return "string";
    case REDIS_REPLY_ARRAY: return "array";
    case REDIS_REPLY_INTEGER: return "integer";
    case REDIS_REPLY_NIL: return "nil";
    case REDIS_REPLY_STATUS: return "status";
    case REDIS_REPLY_ERROR: return "error";
    default: return "unknown redis type";
    }
}

bool RedisReply::ConsumePartialIOBuf(base::IOBuf& buf, base::Arena* arena) {
    if (_type == REDIS_REPLY_ARRAY && _data.array.last_index >= 0) {
        // The parsing was suspended while parsing sub replies,
        // continue the parsing.
        RedisReply* subs = (RedisReply*)_data.array.replies;
        for (uint32_t i = _data.array.last_index; i < _length; ++i) {
            if (!subs[i].ConsumePartialIOBuf(buf, arena)) {
                return false;
            }
            ++_data.array.last_index;
        }
        // We've got an intact reply. reset the index.
        _data.array.last_index = -1;
        return true;
    }

    // Notice that all branches returning false must not change `buf'.
    const char* pfc = (const char*)buf.fetch1();
    if (pfc == NULL) {
        return false;
    }
    const char fc = *pfc;  // first character
    switch (fc) {
    case '-':   // Error          "-<message>\r\n"
    case '+': { // Simple String  "+<string>\r\n"
        base::IOBuf str;
        if (buf.cut_until(&str, "\r\n") != 0) {
            return false;
        }
        const size_t len = str.size() - 1;
        if (len < sizeof(_data.short_str)) {
            // SSO short strings, including empty string.
            _type = (fc == '-' ? REDIS_REPLY_ERROR : REDIS_REPLY_STATUS);
            _length = len;
            str.copy_to_cstr(_data.short_str, (size_t)-1L, 1/*skip fc*/);
            return true;
        }
        char* d = (char*)arena->allocate((len/8 + 1)*8);
        if (d == NULL) {
            LOG(FATAL) << "Fail to allocate string[" << len << "]";
            return false;
        }
        CHECK_EQ(len, str.copy_to_cstr(d, (size_t)-1L, 1/*skip fc*/));
        _type = (fc == '-' ? REDIS_REPLY_ERROR : REDIS_REPLY_STATUS);
        _length = len;
        _data.long_str = d;
        return true;
    }
    case '$':   // Bulk String   "$<length>\r\n<string>\r\n"
    case '*':   // Array         "*<size>\r\n<sub-reply1><sub-reply2>..."
    case ':': { // Integer       ":<integer>\r\n"
        char intbuf[32];  // enough for fc + 64-bit decimal + \r\n
        const size_t ncopied = buf.copy_to(intbuf, sizeof(intbuf) - 1);
        intbuf[ncopied] = '\0';
        const size_t crlf_pos = base::StringPiece(intbuf, ncopied).find("\r\n");
        if (crlf_pos == base::StringPiece::npos) {  // not enough data
            return false;
        }
        char* endptr = NULL;
        int64_t value = strtoll(intbuf + 1/*skip fc*/, &endptr, 10);
        if (endptr != intbuf + crlf_pos) {
            LOG(ERROR) << '`' << intbuf + 1 << "' is not a valid 64-bit decimal";
            return false;
        }
        if (fc == ':') {
            buf.pop_front(crlf_pos + 2/*CRLF*/);
            _type = REDIS_REPLY_INTEGER;
            _length = 0;
            _data.integer = value;
            return true;
        } else if (fc == '$') {
            const int64_t len = value;  // `value' is length of the string
            if (len < 0) {  // redis nil
                buf.pop_front(crlf_pos + 2/*CRLF*/);
                _type = REDIS_REPLY_NIL;
                _length = 0;
                _data.integer = 0;
                return true;
            }
            if (len > (int64_t)std::numeric_limits<uint32_t>::max()) {
                LOG(ERROR) << "bulk string is too long! max length=2^32-1,"
                    " actually=" << len;
                return false;
            }
            // We provide c_str(), thus even if bulk string is started with
            // length, we have to end it with \0.
            if (buf.size() < crlf_pos + 2 + (size_t)len + 2/*CRLF*/) {
                return false;
            }
            if ((size_t)len < sizeof(_data.short_str)) {
                // SSO short strings, including empty string.
                _type = REDIS_REPLY_STRING;
                _length = len;
                buf.pop_front(crlf_pos + 2);
                buf.cutn(_data.short_str, len);
                _data.short_str[len] = '\0';
            } else {
                char* d = (char*)arena->allocate((len/8 + 1)*8);
                if (d == NULL) {
                    LOG(FATAL) << "Fail to allocate string[" << len << "]";
                    return false;
                }
                buf.pop_front(crlf_pos + 2/*CRLF*/);
                buf.cutn(d, len);
                d[len] = '\0';
                _type = REDIS_REPLY_STRING;
                _length = len;
                _data.long_str = d;
            }
            char crlf[2];
            buf.cutn(crlf, sizeof(crlf));
            if (crlf[0] != '\r' || crlf[1] != '\n') {
                LOG(ERROR) << "Bulk string is not ended with CRLF";
            }
            return true;
        } else {
            const int64_t count = value;  // `value' is count of sub replies
            if (count < 0) { // redis nil
                buf.pop_front(crlf_pos + 2/*CRLF*/);
                _type = REDIS_REPLY_NIL;
                _length = 0;
                _data.integer = 0;
                return true;
            }
            if (count == 0) { // empty array
                buf.pop_front(crlf_pos + 2/*CRLF*/);
                _type = REDIS_REPLY_ARRAY;
                _length = 0;
                _data.array.last_index = -1;
                _data.array.replies = NULL;
                return true;
            }
            if (count > (int64_t)std::numeric_limits<uint32_t>::max()) {
                LOG(ERROR) << "Too many sub replies! max count=2^32-1,"
                    " actually=" << count;
                return false;
            }
            // FIXME(gejun): Call allocate_aligned instead.
            RedisReply* subs = (RedisReply*)arena->allocate(sizeof(RedisReply) * count);
            if (subs == NULL) {
                LOG(FATAL) << "Fail to allocate RedisReply[" << count << "]";
                return false;
            }
            for (int64_t i = 0; i < count; ++i) {
                new (&subs[i]) RedisReply;
            }
            buf.pop_front(crlf_pos + 2/*CRLF*/);
            _type = REDIS_REPLY_ARRAY;
            _length = count;
            _data.array.replies = subs;

            // Resursively parse sub replies. If any of them fails, it will
            // be continued in next calls by tracking _data.array.last_index.
            _data.array.last_index = 0;
            for (int64_t i = 0; i < count; ++i) {
                if (!subs[i].ConsumePartialIOBuf(buf, arena)) {
                    return false;
                }
                ++_data.array.last_index;
            }
            _data.array.last_index = -1;
            return true;
        }
    }
    default:
        LOG(ERROR) << "Invalid first character=" << (int)fc;
        return false;
    }
    return false;
}

static void PrintBinaryData(std::ostream& os, const base::StringPiece& s) {
    // Check for non-ascii characters first so that we can print ascii data
    // (most cases) fast, rather than printing char-by-char as we do in the
    // binary_data=true branch.
    bool binary_data = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] <= 0) {
            binary_data = true;
            break;
        }
    }
    if (!binary_data) {
        os << s;
    } else {
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] <= 0) {
                char buf[8] = "\\u0000";
                uint8_t d1 = ((uint8_t)s[i]) & 0xF;
                uint8_t d2 = ((uint8_t)s[i]) >> 4;
                buf[4] = (d1 < 10 ? d1 + '0' : (d1 - 10) + 'A');
                buf[5] = (d2 < 10 ? d2 + '0' : (d2 - 10) + 'A');
                os << base::StringPiece(buf, 6);
            } else {
                os << s[i];
            }
        }
    }
}

// Mimic how official redis-cli prints.
void RedisReply::Print(std::ostream& os) const {
    switch (_type) {
    case REDIS_REPLY_STRING:
        os << '"';
        if (_length < sizeof(_data.short_str)) {
            os << _data.short_str;
        } else {
            PrintBinaryData(os, base::StringPiece(_data.long_str, _length));
        }
        os << '"';
        break;
    case REDIS_REPLY_ARRAY:
        os << '[';
        for (uint32_t i = 0; i < _length; ++i) {
            if (i != 0) {
                os << ", ";
            }
            _data.array.replies[i].Print(os);
        }
        os << ']';
        break;
    case REDIS_REPLY_INTEGER:
        os << "(integer) " << _data.integer;
        break;
    case REDIS_REPLY_NIL:
        os << "(nil)";
        break;
    case REDIS_REPLY_ERROR:
        os << "(error) ";
        // fall through
    case REDIS_REPLY_STATUS:
        if (_length < sizeof(_data.short_str)) {
            os << _data.short_str;
        } else {
            PrintBinaryData(os, base::StringPiece(_data.long_str, _length));
        }
        break;
    default:
        os << "UnknownType=" << _type;
        break;
    }
}

} // namespace brpc
