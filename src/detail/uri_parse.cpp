// Copyright 2016 Glyn Matthews.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#include <cstdlib>
#include <cctype>
#include <iterator>
#include <limits>
#include "grammar.hpp"
#include <network/uri/uri.hpp>

namespace network {
namespace {
inline string_view uri_part(string_view::const_iterator first,
                            string_view::const_iterator last) {
  const char *ptr = &(*first);
  return string_view(ptr, last - first);
}

enum class uri_state {
  scheme,
  hier_part,
  query,
  fragment
};

enum class hier_part_state {
  first_slash,
  second_slash,
  authority,
  host,
  host_ipv6,
  port,
  path
};

enum class authority_state {
  start,
  host,
  host_ipv6,
  port
};

bool validate_scheme(string_view::const_iterator &it,
                     string_view::const_iterator last) {
  if (it == last) {
    return false;
  }

  // The first character must be a letter
  if (!std::isalpha(*it, std::locale("C"))) {
    return false;
  }
  ++it;

  while (it != last) {
    if (*it == ':') {
      break;
    }
    else if (!detail::isalnum(it, last) && !detail::is_in(it, last, "+-.")) {
      return false;
    }
  }

  return true;
}

bool is_valid_user_info(string_view::const_iterator it,
                        string_view::const_iterator last) {
  while (it != last) {
    if (!detail::is_unreserved(it, last) &&
        !detail::is_pct_encoded(it, last) &&
        !detail::is_sub_delim(it, last) &&
        !detail::is_in(it, last, ":")) {
      return false;
    }
  }
  return true;
}

bool set_host_and_port(string_view::const_iterator first,
                       string_view::const_iterator last,
                       string_view::const_iterator last_colon,
                       uri_parts &parts) {
  if (first == last_colon) {
    parts.host = uri_part(first, last);
  }
  else {
    auto port_start = last_colon;
    ++port_start;
    parts.host = uri_part(first, last_colon);
    if (!detail::is_valid_port(port_start)) {
      return false;
    }
    parts.port = uri_part(port_start, last);
  }
  return true;
}

bool validate_query(string_view::const_iterator &it,
                    string_view::const_iterator last) {
  while (it != last) {
    if (!detail::is_pchar(it, last) && !detail::is_in(it, last, "?/")) {
      return (*it == '#');
    }
  }
  return true;
}

bool validate_fragment(string_view::const_iterator &it,
                       string_view::const_iterator last) {
  while (it != last) {
    if (!detail::is_pchar(it, last) && !detail::is_in(it, last, "?/")) {
      return false;
    }
  }
  return true;
}
} // namespace

bool parse_uri(string_view::const_iterator &it, string_view::const_iterator last,
               uri_parts &parts) {
  auto state = uri_state::scheme;

  auto first = it;

  if (it == last) {
    return false;
  }

  if (validate_scheme(it, last)) {
    parts.scheme = uri_part(first, it);
    // move past the scheme delimiter
    ++it;
    state = uri_state::hier_part;
  }
  else {
    return false;
  }

  // Hierarchical part
  auto hp_state = hier_part_state::first_slash;
  // this is used by the user_info/port
  auto last_colon = first;
  while (it != last) {
    if (hp_state == hier_part_state::first_slash) {
      if (*it == '/') {
        hp_state = hier_part_state::second_slash;
        // set the first iterator in case the second slash is not forthcoming
        first = it;
        ++it;
        continue;
      }
      else {
        hp_state = hier_part_state::path;
        first = it;
      }
    }
    else if (hp_state == hier_part_state::second_slash) {
      if (*it == '/') {
        hp_state = hier_part_state::authority;
        ++it;
        first = it;
        continue;
      }
      else {
        // it's a valid URI, and this is the beginning of the path
        hp_state = hier_part_state::path;
      }
    }
    else if (hp_state == hier_part_state::authority) {
      if (detail::is_in(first, last, "@:")) {
        return false;
      }

      // reset the last colon
      if (first == it) {
        last_colon = first;
      }

      if (*it == '@') {
        if (!is_valid_user_info(first, it)) {
          return false;
        }
        parts.user_info = uri_part(first, it);
        hp_state = hier_part_state::host;
        ++it;
        first = it;

        if (*first == '[') {
          // this is an IPv6 address
          hp_state = hier_part_state::host_ipv6;
        }

        continue;
      }
      else if (*it == '[') {
        // this is an IPv6 address
        hp_state = hier_part_state::host_ipv6;
        first = it;
        continue;
      }
      else if (*it == ':') {
        last_colon = it;
      }
      else if (*it == '/') {
        // we skipped right past the host and port, and are at the path.
        if (!set_host_and_port(first, it, last_colon, parts)) {
          return false;
        }
        hp_state = hier_part_state::path;
        first = it;
        continue;
      }
      else if (*it == '?') {
        // the path is empty, but valid, and the next part is the query
        if (!set_host_and_port(first, it, last_colon, parts)) {
          return false;
        }
        parts.path = uri_part(it, it);
        state = uri_state::query;
        ++it;
        first = it;
        break;
      }
      else if (*it == '#') {
        // the path is empty, but valid, and the next part is the fragment
        if (!set_host_and_port(first, it, last_colon, parts)) {
          return false;
        }
        parts.path = uri_part(it, it);
        state = uri_state::fragment;
        ++it;
        first = it;
        break;
      }
    }
    else if (hp_state == hier_part_state::host) {
      if (*first == ':') {
        return false;
      }

      if (*it == ':') {
        parts.host = uri_part(first, it);
        hp_state = hier_part_state::port;
        ++it;
        first = it;
        continue;
      }
      else if (*it == '/') {
        parts.host = uri_part(first, it);
        hp_state = hier_part_state::path;
        first = it;
        continue;
      }
      else if (*it == '?') {
        // the path is empty, but valid, and the next part is the query
        parts.host = uri_part(first, it);
        parts.path = uri_part(it, it);
        state = uri_state::query;
        ++it;
        first = it;
        break;
      }
      else if (*it == '#') {
        // the path is empty, but valid, and the next part is the fragment
        parts.host = uri_part(first, it);
        parts.path = uri_part(it, it);
        state = uri_state::fragment;
        ++it;
        first = it;
        break;
      }
    }
    else if (hp_state == hier_part_state::host_ipv6) {
      if (*first != '[') {
        return false;
      }

      if (*it == ']') {
        ++it;
        // Then test if the next part is a host, part, or the end of the file
        if (it == last) {
          break;
        }
        else if (*it == ':') {
          parts.host = uri_part(first, it);
          hp_state = hier_part_state::port;
          ++it;
          first = it;
        }
        else if (*it == '/') {
          parts.host = uri_part(first, it);
          hp_state = hier_part_state::path;
          first = it;
        }
        else if (*it == '?') {
          parts.host = uri_part(first, it);
          parts.path = uri_part(it, it);
          state = uri_state::query;
          ++it;
          first = it;
          break;
        }
        else if (*it == '#') {
          parts.host = uri_part(first, it);
          parts.path = uri_part(it, it);
          state = uri_state::fragment;
          ++it;
          first = it;
          break;
        }
        continue;
      }
    }
    else if (hp_state == hier_part_state::port) {
      if (*first == '/') {
        // the port is empty, but valid
        if (!detail::is_valid_port(first)) {
          return false;
        }
        parts.port = uri_part(first, it);

        // the port isn't set, but the path is
        hp_state = hier_part_state::path;
        continue;
      }

      if (*it == '/') {
        if (!detail::is_valid_port(first)) {
          return false;
        }
        parts.port = uri_part(first, it);
        hp_state = hier_part_state::path;
        first = it;
        continue;
      }
      else if (!detail::isdigit(it, last)) {
        return false;
      }
    }
    else if (hp_state == hier_part_state::path) {
      if (*it == '?') {
        parts.path = uri_part(first, it);
        // move past the query delimiter
        ++it;
        first = it;
        state = uri_state::query;
        break;
      }
      else if (*it == '#') {
        parts.path = uri_part(first, it);
        // move past the fragment delimiter
        ++it;
        first = it;
        state = uri_state::fragment;
        break;
      }

      if (!detail::is_pchar(it, last) && !detail::is_in(it, last, "/")) {
        return false;
      }
      else {
        continue;
      }
    }

    ++it;
  }

  if (state == uri_state::query) {
    if (!validate_query(it, last)) {
      return false;
    }

    if (*it == '#') {
      parts.query = uri_part(first, it);
      // move past the fragment delimiter
      ++it;
      first = it;
      state = uri_state::fragment;
    }
  }

  if (state == uri_state::fragment) {
    if (!validate_fragment(it, last)) {
      return false;
    }
  }

  // we're done!
  if (state == uri_state::hier_part) {
    if (hp_state == hier_part_state::authority) {
      if (!set_host_and_port(first, last, last_colon, parts)) {
        return false;
      }
      parts.path = uri_part(last, last);
    }
    else if (hp_state == hier_part_state::host) {
      if (!set_host_and_port(first, last, last_colon, parts)) {
        return false;
      }
      parts.path = uri_part(last, last);
    }
    else if (hp_state == hier_part_state::host_ipv6) {
      if (!set_host_and_port(first, last, last_colon, parts)) {
        return false;
      }
      parts.path = uri_part(last, last);
    }
    else if (hp_state == hier_part_state::port) {
      if (!detail::is_valid_port(first)) {
        return false;
      }
      parts.port = uri_part(first, last);
      parts.path = uri_part(last, last);
    }
    else if (hp_state == hier_part_state::path) {
      parts.path = uri_part(first, last);
    }
  }
  else if (state == uri_state::query) {
    parts.query = uri_part(first, last);
  }
  else if (state == uri_state::fragment) {
    parts.fragment = uri_part(first, last);
  }

  return true;
}
}  // namespace network
