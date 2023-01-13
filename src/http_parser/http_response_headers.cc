// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// The rules for header parsing were borrowed from Firefox:
// http://lxr.mozilla.org/seamonkey/source/netwerk/protocol/http/src/nsHttpResponseHead.cpp
// The rules for parsing content-types were also borrowed from Firefox:
// http://lxr.mozilla.org/mozilla/source/netwerk/base/src/nsURLHelper.cpp#834
#include "http_parser/http_response_headers.hh"
#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include "quic/platform/api/quic_logging.h"
#include "platform/quiche_platform_impl/quiche_logging_impl.h"
#include "googleurl/base/stl_util.h"
#include "googleurl/base/strings/strcat.h"
#include "googleurl/base/strings/string_number_conversions.h"
#include "googleurl/base/strings/string_piece.h"
#include "googleurl/base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "common/platform/api/quiche_logging.h"
#include "common/platform/api/quiche_text_utils.h"
#include "quic/core/quic_time.h"
#include "net/base/escape.h"
#include "net/base/parse_number.h"
#include "http_parser/http_byte_range.h"
#include "http_parser/http_util.hh"
#include "absl/strings/str_format.h"
using namespace quic;
namespace bvc {
namespace {
// These headers are RFC 2616 hop-by-hop headers;
// not to be stored by caches.
const char* const kHopByHopResponseHeaders[] = {
  "connection",
  "proxy-connection",
  "keep-alive",
  "trailer",
  "transfer-encoding",
  "upgrade"
};

// These headers are challenge response headers;
// not to be stored by caches.
const char* const kChallengeResponseHeaders[] = {
  "www-authenticate",
  "proxy-authenticate"
};

// These headers are cookie setting headers;
// not to be stored by caches or disclosed otherwise.
const char* const kCookieResponseHeaders[] = {
  "set-cookie",
  "set-cookie2",
  "clear-site-data",
};

// By default, do not cache Strict-Transport-Security.
// This avoids erroneously re-processing it on page loads from cache ---
// it is defined to be valid only on live and error-free HTTPS connections.
const char* const kSecurityStateHeaders[] = {
  "strict-transport-security",
};

// These response headers are not copied from a 304/206 response to the cached
// response headers.  This list is based on Mozilla's nsHttpResponseHead.cpp.
const char* const kNonUpdatedHeaders[] = {
    "connection",
    "proxy-connection",
    "keep-alive",
    "www-authenticate",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
    "content-location",
    "content-md5",
    "etag",
    "content-encoding",
    "content-range",
    "content-type",
    "content-length",
    "x-frame-options",
    "x-xss-protection",
};

// Some header prefixes mean "Don't copy this header from a 304 response.".
// Rather than listing all the relevant headers, we can consolidate them into
// this list:
const char* const kNonUpdatedHeaderPrefixes[] = {
  "x-content-",
  "x-webkit-"
};

bool ShouldUpdateHeader(gurl_base::StringPiece name) {
  for (size_t i = 0; i < gurl_base::size(kNonUpdatedHeaders); ++i) {
    if (gurl_base::LowerCaseEqualsASCII(name, kNonUpdatedHeaders[i]))
      return false;
  }
  for (size_t i = 0; i < gurl_base::size(kNonUpdatedHeaderPrefixes); ++i) {
    if (gurl_base::StartsWith(name, kNonUpdatedHeaderPrefixes[i],
                         gurl_base::CompareCase::INSENSITIVE_ASCII))
      return false;
  }
  return true;
}

bool HasEmbeddedNulls(gurl_base::StringPiece str) {
  for (char c : str) {
    if (c == '\0')
      return true;
  }
  return false;
}
void CheckDoesNotHaveEmbeddedNulls(gurl_base::StringPiece str) {
  // Care needs to be taken when adding values to the raw headers string to
  // make sure it does not contain embeded NULLs. Any embeded '\0' may be
  // understood as line terminators and change how header lines get tokenized.
  QUICHE_CHECK(!HasEmbeddedNulls(str));
}
}  // namespace
const char HttpResponseHeaders::kContentRange[] = "Content-Range";
struct HttpResponseHeaders::ParsedHeader {
  // A header "continuation" contains only a subsequent value for the
  // preceding header.  (Header values are comma separated.)
  bool is_continuation() const { return name_begin == name_end; }
  std::string::const_iterator name_begin;
  std::string::const_iterator name_end;
  std::string::const_iterator value_begin;
  std::string::const_iterator value_end;
};
HttpResponseHeaders::HttpResponseHeaders(const std::string& raw_input)
    : response_code_(-1) {
  Parse(raw_input);
  // The most important thing to do with this histogram is find out
  // the existence of unusual HTTP status codes.  As it happens
  // right now, there aren't double-constructions of response headers
  // using this constructor, so our counts should also be accurate,
  // without instantiating the histogram in two places.  It is also
  // important that this histogram not collect data in the other
  // constructor, which rebuilds an histogram from a pickle, since
  // that would actually create a double call between the original
  // HttpResponseHeader that was serialized, and initialization of the
  // new object from that pickle.
}

std::shared_ptr<HttpResponseHeaders> HttpResponseHeaders::TryToCreate(
    gurl_base::StringPiece headers) {
  // Reject strings with nulls.
  if (HasEmbeddedNulls(headers) ||
      headers.size() > std::numeric_limits<int>::max()) {
    return nullptr;
  }
  return std::make_shared<HttpResponseHeaders>(
      HttpUtil::AssembleRawHeaders(headers));
}

void HttpResponseHeaders::Update(const HttpResponseHeaders& new_headers) {
  QUICHE_DCHECK(new_headers.response_code() == 304 ||
         new_headers.response_code() == 206);
  // Copy up to the null byte.  This just copies the status line.
  std::string new_raw_headers(raw_headers_.c_str());
  new_raw_headers.push_back('\0');
  HeaderSet updated_headers;
  // NOTE: we write the new headers then the old headers for convenience.  The
  // order should not matter.
  // Figure out which headers we want to take from new_headers:
  for (size_t i = 0; i < new_headers.parsed_.size(); ++i) {
    const HeaderList& new_parsed = new_headers.parsed_;
    QUICHE_DCHECK(!new_parsed[i].is_continuation());
    // Locate the start of the next header.
    size_t k = i;
    while (++k < new_parsed.size() && new_parsed[k].is_continuation()) {}
    --k;
    gurl_base::StringPiece name(new_parsed[i].name_begin, new_parsed[i].name_end);
    if (ShouldUpdateHeader(name)) {
      std::string name_lower = gurl_base::ToLowerASCII(name);
      updated_headers.insert(name_lower);
      // Preserve this header line in the merged result, making sure there is
      // a null after the value.
      new_raw_headers.append(new_parsed[i].name_begin, new_parsed[k].value_end);
      new_raw_headers.push_back('\0');
    }
    i = k;
  }
  // Now, build the new raw headers.
  MergeWithHeaders(new_raw_headers, updated_headers);
}
void HttpResponseHeaders::MergeWithHeaders(const std::string& raw_headers,
                                           const HeaderSet& headers_to_remove) {
  std::string new_raw_headers(raw_headers);
  for (size_t i = 0; i < parsed_.size(); ++i) {
    QUICHE_DCHECK(!parsed_[i].is_continuation());
    // Locate the start of the next header.
    size_t k = i;
    while (++k < parsed_.size() && parsed_[k].is_continuation()) {}
    --k;
    std::string name = gurl_base::ToLowerASCII(
        gurl_base::StringPiece(parsed_[i].name_begin, parsed_[i].name_end));
    if (headers_to_remove.find(name) == headers_to_remove.end()) {
      // It's ok to preserve this header in the final result.
      new_raw_headers.append(parsed_[i].name_begin, parsed_[k].value_end);
      new_raw_headers.push_back('\0');
    }
    i = k;
  }
  new_raw_headers.push_back('\0');
  // Make this object hold the new data.
  raw_headers_.clear();
  parsed_.clear();
  Parse(new_raw_headers);
}
void HttpResponseHeaders::RemoveHeader(gurl_base::StringPiece name) {
  // Copy up to the null byte.  This just copies the status line.
  std::string new_raw_headers(raw_headers_.c_str());
  new_raw_headers.push_back('\0');
  HeaderSet to_remove;
  to_remove.insert(gurl_base::ToLowerASCII(name));
  MergeWithHeaders(new_raw_headers, to_remove);
}
void HttpResponseHeaders::RemoveHeaders(
    const std::unordered_set<std::string>& header_names) {
  // Copy up to the null byte.  This just copies the status line.
  std::string new_raw_headers(raw_headers_.c_str());
  new_raw_headers.push_back('\0');
  HeaderSet to_remove;
  for (const auto& header_name : header_names) {
    to_remove.insert(gurl_base::ToLowerASCII(header_name));
  }
  MergeWithHeaders(new_raw_headers, to_remove);
}
void HttpResponseHeaders::RemoveHeaderLine(const std::string& name,
                                           const std::string& value) {
  std::string name_lowercase = gurl_base::ToLowerASCII(name);
  std::string new_raw_headers(GetStatusLine());
  new_raw_headers.push_back('\0');
  new_raw_headers.reserve(raw_headers_.size());
  size_t iter = 0;
  std::string old_header_name;
  std::string old_header_value;
  while (EnumerateHeaderLines(&iter, &old_header_name, &old_header_value)) {
    std::string old_header_name_lowercase = gurl_base::ToLowerASCII(old_header_name);
    if (name_lowercase == old_header_name_lowercase &&
        value == old_header_value)
      continue;
    new_raw_headers.append(old_header_name);
    new_raw_headers.push_back(':');
    new_raw_headers.push_back(' ');
    new_raw_headers.append(old_header_value);
    new_raw_headers.push_back('\0');
  }
  new_raw_headers.push_back('\0');
  // Make this object hold the new data.
  raw_headers_.clear();
  parsed_.clear();
  Parse(new_raw_headers);
}
void HttpResponseHeaders::AddHeader(gurl_base::StringPiece name,
                                    gurl_base::StringPiece value) {
  QUICHE_DCHECK(HttpUtil::IsValidHeaderName(name));
  QUICHE_DCHECK(HttpUtil::IsValidHeaderValue(value));
  // Don't copy the last null.
  std::string new_raw_headers(raw_headers_, 0, raw_headers_.size() - 1);
  new_raw_headers.append(name.begin(), name.end());
  new_raw_headers.append(": ");
  new_raw_headers.append(value.begin(), value.end());
  new_raw_headers.push_back('\0');
  new_raw_headers.push_back('\0');
  // Make this object hold the new data.
  raw_headers_.clear();
  parsed_.clear();
  Parse(new_raw_headers);
}
void HttpResponseHeaders::SetHeader(gurl_base::StringPiece name,
                                    gurl_base::StringPiece value) {
  RemoveHeader(name);
  AddHeader(name, value);
}
void HttpResponseHeaders::AddCookie(const std::string& cookie_string) {
  AddHeader("Set-Cookie", cookie_string);
}
void HttpResponseHeaders::ReplaceStatusLine(const std::string& new_status) {
  CheckDoesNotHaveEmbeddedNulls(new_status);
  // Copy up to the null byte.  This just copies the status line.
  std::string new_raw_headers(new_status);
  new_raw_headers.push_back('\0');
  HeaderSet empty_to_remove;
  MergeWithHeaders(new_raw_headers, empty_to_remove);
}
void HttpResponseHeaders::UpdateWithNewRange(const HttpByteRange& byte_range,
                                             int64_t resource_size,
                                             bool replace_status_line) {
  QUICHE_DCHECK(byte_range.IsValid());
  QUICHE_DCHECK(byte_range.HasFirstBytePosition());
  QUICHE_DCHECK(byte_range.HasLastBytePosition());
  const char kLengthHeader[] = "Content-Length";
  const char kRangeHeader[] = "Content-Range";
  RemoveHeader(kLengthHeader);
  RemoveHeader(kRangeHeader);
  int64_t start = byte_range.first_byte_position();
  int64_t end = byte_range.last_byte_position();
  int64_t range_len = end - start + 1;
  if (replace_status_line)
    ReplaceStatusLine("HTTP/1.1 206 Partial Content");
  AddHeader(kRangeHeader,
            absl::StrFormat("bytes %d-%d/%d", start,
                               end, resource_size));
  AddHeader(kLengthHeader, absl::StrFormat("%d", range_len));
}
void HttpResponseHeaders::Parse(const std::string& raw_input) {
  raw_headers_.reserve(raw_input.size());
  // ParseStatusLine adds a normalized status line to raw_headers_
  std::string::const_iterator line_begin = raw_input.begin();
  std::string::const_iterator line_end =
      std::find(line_begin, raw_input.end(), '\0');
  // has_headers = true, if there is any data following the status line.
  // Used by ParseStatusLine() to decide if a HTTP/0.9 is really a HTTP/1.0.
  bool has_headers =
      (line_end != raw_input.end() && (line_end + 1) != raw_input.end() &&
       *(line_end + 1) != '\0');
  ParseStatusLine(line_begin, line_end, has_headers);
  raw_headers_.push_back('\0');  // Terminate status line with a null.
  if (line_end == raw_input.end()) {
    raw_headers_.push_back('\0');  // Ensure the headers end with a double null.
    QUICHE_DCHECK_EQ('\0', raw_headers_[raw_headers_.size() - 2]);
    QUICHE_DCHECK_EQ('\0', raw_headers_[raw_headers_.size() - 1]);
    return;
  }
  // Including a terminating null byte.
  size_t status_line_len = raw_headers_.size();
  // Now, we add the rest of the raw headers to raw_headers_, and begin parsing
  // it (to populate our parsed_ vector).
  raw_headers_.append(line_end + 1, raw_input.end());
  // Ensure the headers end with a double null.
  while (raw_headers_.size() < 2 ||
         raw_headers_[raw_headers_.size() - 2] != '\0' ||
         raw_headers_[raw_headers_.size() - 1] != '\0') {
    raw_headers_.push_back('\0');
  }
  // Adjust to point at the null byte following the status line
  line_end = raw_headers_.begin() + status_line_len - 1;
  HttpUtil::HeadersIterator headers(line_end + 1, raw_headers_.end(),
                                    std::string(1, '\0'));
  while (headers.GetNext()) {
    AddHeader(headers.name_begin(), headers.name_end(), headers.values_begin(),
              headers.values_end());
  }
  QUICHE_DCHECK_EQ('\0', raw_headers_[raw_headers_.size() - 2]);
  QUICHE_DCHECK_EQ('\0', raw_headers_[raw_headers_.size() - 1]);
}
bool HttpResponseHeaders::GetNormalizedHeader(gurl_base::StringPiece name,
                                              std::string* value) const {
  // If you hit this assertion, please use EnumerateHeader instead!
  QUICHE_DCHECK(!HttpUtil::IsNonCoalescingHeader(name));
  value->clear();
  bool found = false;
  size_t i = 0;
  while (i < parsed_.size()) {
    i = FindHeader(i, name);
    if (i == std::string::npos)
      break;
    if (found)
      value->append(", ");
    found = true;
    std::string::const_iterator value_begin = parsed_[i].value_begin;
    std::string::const_iterator value_end = parsed_[i].value_end;
    while (++i < parsed_.size() && parsed_[i].is_continuation())
      value_end = parsed_[i].value_end;
    value->append(value_begin, value_end);
  }
  return found;
}
std::string HttpResponseHeaders::GetStatusLine() const {
  // copy up to the null byte.
  return std::string(raw_headers_.c_str());
}
std::string HttpResponseHeaders::GetStatusText() const {
  // GetStatusLine() is already normalized, so it has the format:
  // '<http_version> SP <response_code>' or
  // '<http_version> SP <response_code> SP <status_text>'.
  std::string status_text = GetStatusLine();
  std::string::const_iterator begin = status_text.begin();
  std::string::const_iterator end = status_text.end();
  // Seek to beginning of <response_code>.
  begin = std::find(begin, end, ' ');
  QUICHE_CHECK(begin != end);
  ++begin;
  QUICHE_CHECK(begin != end);
  // See if there is another space.
  begin = std::find(begin, end, ' ');
  if (begin == end)
    return std::string();
  ++begin;
  QUICHE_CHECK(begin != end);
  return std::string(begin, end);
}
bool HttpResponseHeaders::EnumerateHeaderLines(size_t* iter,
                                               std::string* name,
                                               std::string* value) const {
  size_t i = *iter;
  if (i == parsed_.size())
    return false;
  QUICHE_DCHECK(!parsed_[i].is_continuation());
  name->assign(parsed_[i].name_begin, parsed_[i].name_end);
  std::string::const_iterator value_begin = parsed_[i].value_begin;
  std::string::const_iterator value_end = parsed_[i].value_end;
  while (++i < parsed_.size() && parsed_[i].is_continuation())
    value_end = parsed_[i].value_end;
  value->assign(value_begin, value_end);
  *iter = i;
  return true;
}
bool HttpResponseHeaders::EnumerateHeader(size_t* iter,
                                          gurl_base::StringPiece name,
                                          std::string* value) const {
  size_t i;
  if (!iter || !*iter) {
    i = FindHeader(0, name);
  } else {
    i = *iter;
    if (i >= parsed_.size()) {
      i = std::string::npos;
    } else if (!parsed_[i].is_continuation()) {
      i = FindHeader(i, name);
    }
  }
  if (i == std::string::npos) {
    value->clear();
    return false;
  }
  if (iter)
    *iter = i + 1;
  value->assign(parsed_[i].value_begin, parsed_[i].value_end);
  return true;
}
bool HttpResponseHeaders::HasHeaderValue(gurl_base::StringPiece name,
                                         gurl_base::StringPiece value) const {
  // The value has to be an exact match.  This is important since
  // 'cache-control: no-cache' != 'cache-control: no-cache="foo"'
  size_t iter = 0;
  std::string temp;
  while (EnumerateHeader(&iter, name, &temp)) {
    if (gurl_base::EqualsCaseInsensitiveASCII(value, temp))
      return true;
  }
  return false;
}
bool HttpResponseHeaders::HasHeader(gurl_base::StringPiece name) const {
  return FindHeader(0, name) != std::string::npos;
}
HttpResponseHeaders::~HttpResponseHeaders() = default;
// Note: this implementation implicitly assumes that line_end points at a valid
// sentinel character (such as '\0').
// static
HttpVersion HttpResponseHeaders::ParseVersion(
    std::string::const_iterator line_begin,
    std::string::const_iterator line_end) {
  std::string::const_iterator p = line_begin;
  // RFC2616 sec 3.1: HTTP-Version   = "HTTP" "/" 1*DIGIT "." 1*DIGIT
  // TODO: (1*DIGIT apparently means one or more digits, but we only handle 1).
  // TODO: handle leading zeros, which is allowed by the rfc1616 sec 3.1.
  if (!gurl_base::StartsWith(gurl_base::StringPiece(line_begin, line_end), "http",
                        gurl_base::CompareCase::INSENSITIVE_ASCII)) {
    QUIC_LOG(DFATAL) << "missing status line";
    return HttpVersion();
  }
  p += 4;
  if (p >= line_end || *p != '/') {
    QUIC_LOG(DFATAL) << "missing version";
    return HttpVersion();
  }
  std::string::const_iterator dot = std::find(p, line_end, '.');
  if (dot == line_end) {
    QUIC_LOG(DFATAL) << "malformed version";
    return HttpVersion();
  }
  ++p;  // from / to first digit.
  ++dot;  // from . to second digit.
  if (!(gurl_base::IsAsciiDigit(*p) && gurl_base::IsAsciiDigit(*dot))) {
    QUIC_LOG(DFATAL) << "malformed version number";
    return HttpVersion();
  }
  uint16_t major = *p - '0';
  uint16_t minor = *dot - '0';
  return HttpVersion(major, minor);
}
// Note: this implementation implicitly assumes that line_end points at a valid
// sentinel character (such as '\0').
void HttpResponseHeaders::ParseStatusLine(
    std::string::const_iterator line_begin,
    std::string::const_iterator line_end,
    bool has_headers) {
  // Extract the version number
  HttpVersion parsed_http_version = ParseVersion(line_begin, line_end);
  // Clamp the version number to one of: {0.9, 1.0, 1.1, 2.0}
  if (parsed_http_version == HttpVersion(0, 9) && !has_headers) {
    http_version_ = HttpVersion(0, 9);
    raw_headers_ = "HTTP/0.9";
  } else if (parsed_http_version == HttpVersion(2, 0)) {
    http_version_ = HttpVersion(2, 0);
    raw_headers_ = "HTTP/2.0";
  } else if (parsed_http_version >= HttpVersion(1, 1)) {
    http_version_ = HttpVersion(1, 1);
    raw_headers_ = "HTTP/1.1";
  } else {
    // Treat everything else like HTTP 1.0
    http_version_ = HttpVersion(1, 0);
    raw_headers_ = "HTTP/1.0";
  }
  if (parsed_http_version != http_version_) {
    QUIC_LOG(DFATAL) << "assuming HTTP/" << http_version_.major_value() << "."
             << http_version_.minor_value();
  }
  // TODO(eroman): this doesn't make sense if ParseVersion failed.
  std::string::const_iterator p = std::find(line_begin, line_end, ' ');
  if (p == line_end) {
    QUIC_LOG(DFATAL) << "missing response status; assuming 200 OK";
    raw_headers_.append(" 200 OK");
    response_code_ = 200;
    return;
  }
  // Skip whitespace.
  while (p < line_end && *p == ' ')
    ++p;
  std::string::const_iterator code = p;
  while (p < line_end && gurl_base::IsAsciiDigit(*p))
    ++p;
  if (p == code) {
    QUIC_LOG(DFATAL) << "missing response status number; assuming 200";
    raw_headers_.append(" 200");
    response_code_ = 200;
    return;
  }
  raw_headers_.push_back(' ');
  raw_headers_.append(code, p);
  quiche::QuicheStringPiece code_str(gurl_base::StringPiece(code, p).data());
  quiche::QuicheTextUtilsImpl::StringToInt(code_str, &response_code_);
  // Skip whitespace.
  while (p < line_end && *p == ' ')
    ++p;
  // Trim trailing whitespace.
  while (line_end > p && line_end[-1] == ' ')
    --line_end;
  if (p == line_end)
    return;
  raw_headers_.push_back(' ');
  raw_headers_.append(p, line_end);
}
size_t HttpResponseHeaders::FindHeader(size_t from,
                                       gurl_base::StringPiece search) const {
  for (size_t i = from; i < parsed_.size(); ++i) {
    if (parsed_[i].is_continuation())
      continue;
    gurl_base::StringPiece name(parsed_[i].name_begin, parsed_[i].name_end);
    if (gurl_base::EqualsCaseInsensitiveASCII(search, name))
      return i;
  }
  return std::string::npos;
}
bool HttpResponseHeaders::GetCacheControlDirective(gurl_base::StringPiece directive,
                                                   QuicTime::Delta* result) const {
  gurl_base::StringPiece name("cache-control");
  std::string value;
  size_t directive_size = directive.size();
  size_t iter = 0;
  while (EnumerateHeader(&iter, name, &value)) {
    if (value.size() > directive_size + 1 &&
        gurl_base::StartsWith(value, directive,
                         gurl_base::CompareCase::INSENSITIVE_ASCII) &&
        value[directive_size] == '=') {
      int64_t seconds;
      gurl_base::StringPiece value_str(
          gurl_base::StringPiece(value.begin() + directive_size + 1, value.end()).data());
      HttpUtil::StringToInt64(value_str, &seconds);
      *result = QuicTime::Delta::FromSeconds(seconds);
      return true;
    }
  }
  return false;
}
void HttpResponseHeaders::AddHeader(std::string::const_iterator name_begin,
                                    std::string::const_iterator name_end,
                                    std::string::const_iterator values_begin,
                                    std::string::const_iterator values_end) {
  // If the header can be coalesced, then we should split it up.
  if (values_begin == values_end ||
      HttpUtil::IsNonCoalescingHeader(
          gurl_base::StringPiece(name_begin, name_end))) {
    AddToParsed(name_begin, name_end, values_begin, values_end);
  } else {
    HttpUtil::ValuesIterator it(values_begin, values_end, ',',
                                false /* ignore_empty_values */);
    while (it.GetNext()) {
      AddToParsed(name_begin, name_end, it.value_begin(), it.value_end());
      // clobber these so that subsequent values are treated as continuations
      name_begin = name_end = raw_headers_.end();
    }
  }
}
void HttpResponseHeaders::AddToParsed(std::string::const_iterator name_begin,
                                      std::string::const_iterator name_end,
                                      std::string::const_iterator value_begin,
                                      std::string::const_iterator value_end) {
  ParsedHeader header;
  header.name_begin = name_begin;
  header.name_end = name_end;
  header.value_begin = value_begin;
  header.value_end = value_end;
  parsed_.push_back(header);
}
void HttpResponseHeaders::AddNonCacheableHeaders(HeaderSet* result) const {
  // Add server specified transients.  Any 'cache-control: no-cache="foo,bar"'
  // headers present in the response specify additional headers that we should
  // not store in the cache.
  const char kCacheControl[] = "cache-control";
  const char kPrefix[] = "no-cache=\"";
  const size_t kPrefixLen = sizeof(kPrefix) - 1;
  std::string value;
  size_t iter = 0;
  while (EnumerateHeader(&iter, kCacheControl, &value)) {
    // If the value is smaller than the prefix and a terminal quote, skip
    // it.
    if (value.size() <= kPrefixLen ||
        value.compare(0, kPrefixLen, kPrefix) != 0) {
      continue;
    }
    // if it doesn't end with a quote, then treat as malformed
    if (value[value.size() - 1] != '\"')
      continue;
    // process the value as a comma-separated list of items. Each
    // item can be wrapped by linear white space.
    std::string::const_iterator item = value.begin() + kPrefixLen;
    std::string::const_iterator end = value.end() - 1;
    while (item != end) {
      // Find the comma to compute the length of the current item,
      // and the position of the next one.
      std::string::const_iterator item_next = std::find(item, end, ',');
      std::string::const_iterator item_end = end;
      if (item_next != end) {
        // Skip over comma for next position.
        item_end = item_next;
        item_next++;
      }
      // trim off leading and trailing whitespace in this item.
      HttpUtil::TrimLWS(&item, &item_end);
      // assuming the header is not empty, lowercase and insert into set
      if (item_end > item) {
        result->insert(
            gurl_base::ToLowerASCII(gurl_base::StringPiece(&*item, item_end - item)));
      }
      // Continue to next item.
      item = item_next;
    }
  }
}
void HttpResponseHeaders::AddHopByHopHeaders(HeaderSet* result) {
  for (size_t i = 0; i < gurl_base::size(kHopByHopResponseHeaders); ++i)
    result->insert(std::string(kHopByHopResponseHeaders[i]));
}
void HttpResponseHeaders::AddCookieHeaders(HeaderSet* result) {
  for (size_t i = 0; i < gurl_base::size(kCookieResponseHeaders); ++i)
    result->insert(std::string(kCookieResponseHeaders[i]));
}
void HttpResponseHeaders::AddChallengeHeaders(HeaderSet* result) {
  for (size_t i = 0; i < gurl_base::size(kChallengeResponseHeaders); ++i)
    result->insert(std::string(kChallengeResponseHeaders[i]));
}
void HttpResponseHeaders::AddHopContentRangeHeaders(HeaderSet* result) {
  result->insert(kContentRange);
}
void HttpResponseHeaders::AddSecurityStateHeaders(HeaderSet* result) {
  for (size_t i = 0; i < gurl_base::size(kSecurityStateHeaders); ++i)
    result->insert(std::string(kSecurityStateHeaders[i]));
}
void HttpResponseHeaders::GetMimeTypeAndCharset(std::string* mime_type,
                                                std::string* charset) const {
  mime_type->clear();
  charset->clear();
  std::string name = "content-type";
  std::string value;
  bool had_charset = false;
  size_t iter = 0;
  while (EnumerateHeader(&iter, name, &value))
    HttpUtil::ParseContentType(value, mime_type, charset, &had_charset,
                               nullptr);
}
bool HttpResponseHeaders::GetMimeType(std::string* mime_type) const {
  std::string unused;
  GetMimeTypeAndCharset(mime_type, &unused);
  return !mime_type->empty();
}
bool HttpResponseHeaders::GetCharset(std::string* charset) const {
  std::string unused;
  GetMimeTypeAndCharset(&unused, charset);
  return !charset->empty();
}
bool HttpResponseHeaders::IsRedirect(std::string* location) const {
  if (!IsRedirectResponseCode(response_code_))
    return false;
  // If we lack a Location header, then we can't treat this as a redirect.
  // We assume that the first non-empty location value is the target URL that
  // we want to follow.  TODO(darin): Is this consistent with other browsers?
  size_t i = std::string::npos;
  do {
    i = FindHeader(++i, "location");
    if (i == std::string::npos)
      return false;
    // If the location value is empty, then it doesn't count.
  } while (parsed_[i].value_begin == parsed_[i].value_end);
  if (location) {
    gurl_base::StringPiece location_strpiece(parsed_[i].value_begin,
                                        parsed_[i].value_end);
    // Escape any non-ASCII characters to preserve them.  The server should
    // only be returning ASCII here, but for compat we need to do this.
    //
    // The URL parser escapes things internally, but it expect the bytes to be
    // valid UTF-8, so encoding errors turn into replacement characters before
    // escaping. Escaping here preserves the bytes as-is. See
    // https://crbug.com/942073#c14.
    *location = bvc::EscapeNonASCII(location_strpiece);
  }
  return true;
}
// static
bool HttpResponseHeaders::IsRedirectResponseCode(int response_code) {
  // Users probably want to see 300 (multiple choice) pages, so we don't count
  // them as redirects that need to be followed.
  return (response_code == 301 || response_code == 302 ||
          response_code == 303 || response_code == 307 || response_code == 308);
}
// From RFC 2616 section 13.2.4:
//
// The calculation to determine if a response has expired is quite simple:
//
//   response_is_fresh = (freshness_lifetime > current_age)
//
// Of course, there are other factors that can force a response to always be
// validated or re-fetched.
//
// From RFC 5861 section 3, a stale response may be used while revalidation is
// performed in the background if
//
//   freshness_lifetime + stale_while_revalidate > current_age
//
ValidationType HttpResponseHeaders::RequiresValidation(
    const QuicTime& request_time,
    const QuicTime& response_time,
    const QuicTime& current_time) const {
  FreshnessLifetimes lifetimes = GetFreshnessLifetimes(response_time);
  if (lifetimes.freshness.IsZero() && lifetimes.staleness.IsZero())
    return VALIDATION_SYNCHRONOUS;
  QuicTime::Delta age = GetCurrentAge(request_time, response_time, current_time);
  if (lifetimes.freshness > age)
    return VALIDATION_NONE;
  if (lifetimes.freshness + lifetimes.staleness > age)
    return VALIDATION_ASYNCHRONOUS;
  return VALIDATION_SYNCHRONOUS;
}
// From RFC 2616 section 13.2.4:
//
// The max-age directive takes priority over Expires, so if max-age is present
// in a response, the calculation is simply:
//
//   freshness_lifetime = max_age_value
//
// Otherwise, if Expires is present in the response, the calculation is:
//
//   freshness_lifetime = expires_value - date_value
//
// Note that neither of these calculations is vulnerable to clock skew, since
// all of the information comes from the origin server.
//
// Also, if the response does have a Last-Modified time, the heuristic
// expiration value SHOULD be no more than some fraction of the interval since
// that time. A typical setting of this fraction might be 10%:
//
//   freshness_lifetime = (date_value - last_modified_value) * 0.10
//
// If the stale-while-revalidate directive is present, then it is used to set
// the |staleness| time, unless it overridden by another directive.
//
HttpResponseHeaders::FreshnessLifetimes
HttpResponseHeaders::GetFreshnessLifetimes(const QuicTime& response_time) const {
  FreshnessLifetimes lifetimes;
  // Check for headers that force a response to never be fresh.  For backwards
  // compat, we treat "Pragma: no-cache" as a synonym for "Cache-Control:
  // no-cache" even though RFC 2616 does not specify it.
  if (HasHeaderValue("cache-control", "no-cache") ||
      HasHeaderValue("cache-control", "no-store") ||
      HasHeaderValue("pragma", "no-cache")) {
    return lifetimes;
  }
  // Cache-Control directive must_revalidate overrides stale-while-revalidate.
  bool must_revalidate = HasHeaderValue("cache-control", "must-revalidate");
  if (must_revalidate || !GetStaleWhileRevalidateValue(&lifetimes.staleness)) {
    QUICHE_DCHECK_EQ(QuicTime::Delta::Zero(), lifetimes.staleness);
  }
  // NOTE: "Cache-Control: max-age" overrides Expires, so we only check the
  // Expires header after checking for max-age in GetFreshnessLifetimes.  This
  // is important since "Expires: <date in the past>" means not fresh, but
  // it should not trump a max-age value.
  if (GetMaxAgeValue(&lifetimes.freshness))
    return lifetimes;
  // If there is no Date header, then assume that the server response was
  // generated at the time when we received the response.
  QuicTime date_value = QuicTime::Zero();
  if (!GetDateValue(&date_value))
    date_value = response_time;
  QuicTime expires_value = QuicTime::Zero();
  if (GetExpiresValue(&expires_value)) {
    // The expires value can be a date in the past!
    if (expires_value > date_value) {
      lifetimes.freshness = expires_value - date_value;
      return lifetimes;
    }
    QUICHE_DCHECK_EQ(QuicTime::Delta::Zero(), lifetimes.freshness);
    return lifetimes;
  }
  // From RFC 2616 section 13.4:
  //
  //   A response received with a status code of 200, 203, 206, 300, 301 or 410
  //   MAY be stored by a cache and used in reply to a subsequent request,
  //   subject to the expiration mechanism, unless a cache-control directive
  //   prohibits caching.
  //   ...
  //   A response received with any other status code (e.g. status codes 302
  //   and 307) MUST NOT be returned in a reply to a subsequent request unless
  //   there are cache-control directives or another header(s) that explicitly
  //   allow it.
  //
  // From RFC 2616 section 14.9.4:
  //
  //   When the must-revalidate directive is present in a response received by
  //   a cache, that cache MUST NOT use the entry after it becomes stale to
  //   respond to a subsequent request without first revalidating it with the
  //   origin server. (I.e., the cache MUST do an end-to-end revalidation every
  //   time, if, based solely on the origin server's Expires or max-age value,
  //   the cached response is stale.)
  //
  // https://datatracker.ietf.org/doc/draft-reschke-http-status-308/ is an
  // experimental RFC that adds 308 permanent redirect as well, for which "any
  // future references ... SHOULD use one of the returned URIs."
  if ((response_code_ == 200 || response_code_ == 203 ||
       response_code_ == 206) &&
      !must_revalidate) {
    // TODO(darin): Implement a smarter heuristic.
    QuicTime last_modified_value = QuicTime::Zero();
    if (GetLastModifiedValue(&last_modified_value)) {
      // The last-modified value can be a date in the future!
      if (last_modified_value <= date_value) {
        lifetimes.freshness = QuicTime::Delta::FromMicroseconds((date_value - last_modified_value).ToMicroseconds()/10);
        return lifetimes;
      }
    }
  }
  // These responses are implicitly fresh (unless otherwise overruled):
  if (response_code_ == 300 || response_code_ == 301 || response_code_ == 308 ||
      response_code_ == 410) {
    lifetimes.freshness = QuicTime::Delta::Infinite();
    lifetimes.staleness = QuicTime::Delta::Zero();  // It should never be stale.
    return lifetimes;
  }
  // Our heuristic freshness estimate for this resource is 0 seconds, in
  // accordance with common browser behaviour. However, stale-while-revalidate
  // may still apply.
  QUICHE_DCHECK_EQ(QuicTime::Delta::Zero(), lifetimes.freshness);
  return lifetimes;
}
//
// The following data is used for the age calculation:
//
//    age_value
//
//       The term "age_value" denotes the value of the Age header field
//       (Section 5.1), in a form appropriate for arithmetic operation; or
//       0, if not available.
//
//    date_value
//
//       The term "date_value" denotes the value of the Date header field,
//       in a form appropriate for arithmetic operations.  See Section
//       7.1.1.2 of [RFC7231] for the definition of the Date header field,
//       and for requirements regarding responses without it.
//
//    now
//
//       The term "now" means "the current value of the clock at the host
//       performing the calculation".  A host ought to use NTP ([RFC5905])
//       or some similar protocol to synchronize its clocks to Coordinated
//       Universal Time.
//
//    request_time
//
//       The current value of the clock at the host at the time the request
//       resulting in the stored response was made.
//
//    response_time
//
//       The current value of the clock at the host at the time the
//       response was received.
//
//    The age is then calculated as
//
//     apparent_age = max(0, response_time - date_value);
//     response_delay = response_time - request_time;
//     corrected_age_value = age_value + response_delay;
//     corrected_initial_age = max(apparent_age, corrected_age_value);
//     resident_time = now - response_time;
//     current_age = corrected_initial_age + resident_time;
//
QuicTime::Delta HttpResponseHeaders::GetCurrentAge(const QuicTime& request_time,
                                             const QuicTime& response_time,
                                             const QuicTime& current_time) const {
  // If there is no Date header, then assume that the server response was
  // generated at the time when we received the response.
  QuicTime date_value = QuicTime::Zero();
  if (!GetDateValue(&date_value))
    date_value = response_time;
  // If there is no Age header, then assume age is zero.  GetAgeValue does not
  // modify its out param if the value does not exist.
  QuicTime::Delta age_value = QuicTime::Delta::Zero();
  GetAgeValue(&age_value);
  QuicTime::Delta apparent_age = std::max(QuicTime::Delta::Zero(), response_time - date_value);
  QuicTime::Delta response_delay = response_time - request_time;
  QuicTime::Delta corrected_age_value = age_value + response_delay;
  QuicTime::Delta corrected_initial_age = std::max(apparent_age, corrected_age_value);
  QuicTime::Delta resident_time = current_time - response_time;
  QuicTime::Delta current_age = corrected_initial_age + resident_time;
  return current_age;
}
bool HttpResponseHeaders::GetMaxAgeValue(QuicTime::Delta* result) const {
  return GetCacheControlDirective("max-age", result);
}
bool HttpResponseHeaders::GetAgeValue(QuicTime::Delta* result) const {
  std::string value;
  if (!EnumerateHeader(nullptr, "Age", &value))
    return false;
  // Parse the delta-seconds as 1*DIGIT.
  uint32_t seconds;
  ParseIntError error;
  if (!ParseUint32(value, &seconds, &error)) {
    if (error == ParseIntError::FAILED_OVERFLOW) {
      // If the Age value cannot fit in a uint32_t, saturate it to a maximum
      // value. This is similar to what RFC 2616 says in section 14.6 for how
      // caches should transmit values that overflow.
      seconds = std::numeric_limits<decltype(seconds)>::max();
    } else {
      return false;
    }
  }
  *result = QuicTime::Delta::FromSeconds(seconds);
  return true;
}
bool HttpResponseHeaders::GetDateValue(QuicTime* result) const {
  return GetTimeValuedHeader("Date", result);
}
bool HttpResponseHeaders::GetLastModifiedValue(QuicTime* result) const {
  return GetTimeValuedHeader("Last-Modified", result);
}
bool HttpResponseHeaders::GetExpiresValue(QuicTime* result) const {
  return GetTimeValuedHeader("Expires", result);
}
bool HttpResponseHeaders::GetStaleWhileRevalidateValue(
    QuicTime::Delta* result) const {
  return GetCacheControlDirective("stale-while-revalidate", result);
}
bool HttpResponseHeaders::GetTimeValuedHeader(const std::string& name,
                                              QuicTime* result) const {
  std::string value;
  if (!EnumerateHeader(nullptr, name, &value))
    return false;
  // When parsing HTTP dates it's beneficial to default to GMT because:
  // 1. RFC2616 3.3.1 says times should always be specified in GMT
  // 2. Only counter-example incorrectly appended "UTC" (crbug.com/153759)
  // 3. When adjusting cookie expiration times for clock skew
  //    (crbug.com/135131) this better matches our cookie expiration
  //    time parser which ignores timezone specifiers and assumes GMT.
  // 4. This is exactly what Firefox does.
  // TODO(pauljensen): The ideal solution would be to return false if the
  // timezone could not be understood so as to avoid makeing other calculations
  // based on an incorrect time.  This would require modifying the time
  // library or duplicating the code. (http://crbug.com/158327)
  // TODO(panxw): Currently we don't support this.
  // return QuicTime::FromUTCString(value.c_str(), result);
  return false;
}
// We accept the first value of "close" or "keep-alive" in a Connection or
// Proxy-Connection header, in that order. Obeying "keep-alive" in HTTP/1.1 or
// "close" in 1.0 is not strictly standards-compliant, but we'd like to
// avoid looking at the Proxy-Connection header whenever it is reasonable to do
// so.
// TODO(ricea): Measure real-world usage of the "Proxy-Connection" header,
// with a view to reducing support for it in order to make our Connection header
// handling more RFC 7230 compliant.
bool HttpResponseHeaders::IsKeepAlive() const {
  // NOTE: It is perhaps risky to assume that a Proxy-Connection header is
  // meaningful when we don't know that this response was from a proxy, but
  // Mozilla also does this, so we'll do the same.
  static const char* const kConnectionHeaders[] = {"connection",
                                                   "proxy-connection"};
  struct KeepAliveToken {
    const char* const token;
    bool keep_alive;
  };
  static const KeepAliveToken kKeepAliveTokens[] = {{"keep-alive", true},
                                                    {"close", false}};
  if (http_version_ < HttpVersion(1, 0))
    return false;
  for (const char* header : kConnectionHeaders) {
    size_t iterator = 0;
    std::string token;
    while (EnumerateHeader(&iterator, header, &token)) {
      for (const KeepAliveToken& keep_alive_token : kKeepAliveTokens) {
        if (gurl_base::LowerCaseEqualsASCII(token, keep_alive_token.token))
          return keep_alive_token.keep_alive;
      }
    }
  }
  return http_version_ != HttpVersion(1, 0);
}
// From RFC 2616:
// Content-Length = "Content-Length" ":" 1*DIGIT
int64_t HttpResponseHeaders::GetContentLength() const {
  return GetInt64HeaderValue("content-length");
}
int64_t HttpResponseHeaders::GetInt64HeaderValue(
    const std::string& header) const {
  size_t iter = 0;
  std::string content_length_val;
  if (!EnumerateHeader(&iter, header, &content_length_val))
    return -1;
  if (content_length_val.empty())
    return -1;
  if (content_length_val[0] == '+')
    return -1;
  int64_t result;
  bool ok = HttpUtil::StringToInt64(content_length_val, &result);
  if (!ok || result < 0)
    return -1;
  return result;
}
bool HttpResponseHeaders::GetContentRangeFor206(
    int64_t* first_byte_position,
    int64_t* last_byte_position,
    int64_t* instance_length) const {
  size_t iter = 0;
  std::string content_range_spec;
  if (!EnumerateHeader(&iter, kContentRange, &content_range_spec)) {
    *first_byte_position = *last_byte_position = *instance_length = -1;
    return false;
  }
  return HttpUtil::ParseContentRangeHeaderFor206(
      content_range_spec, first_byte_position, last_byte_position,
      instance_length);
}
bool HttpResponseHeaders::IsChunkEncoded() const {
  // Ignore spurious chunked responses from HTTP/1.0 servers and proxies.
  return GetHttpVersion() >= HttpVersion(1, 1) &&
         HasHeaderValue("Transfer-Encoding", "chunked");
}
bool HttpResponseHeaders::IsCookieResponseHeader(gurl_base::StringPiece name) {
  for (const char* cookie_header : kCookieResponseHeaders) {
    if (gurl_base::EqualsCaseInsensitiveASCII(cookie_header, name))
      return true;
  }
  return false;
}
}  // namespace bvc

