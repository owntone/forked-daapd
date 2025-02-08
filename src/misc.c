/*
 * Copyright (C) 2009-2010 Julien BLACHE <jb@jblache.org>
 *
 * Some code included below is in the public domain, check comments
 * in the file.
 *
 * Pieces of code adapted from mt-daapd:
 * Copyright (C) 2003-2007 Ron Pedde (ron@pedde.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/types.h>
#ifndef CLOCK_REALTIME
#include <sys/time.h>
#endif
#ifdef HAVE_UUID
#include <uuid/uuid.h>
#endif
#ifdef HAVE_PTHREAD_NP_H
# include <pthread_np.h>
#endif

#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ifaddrs.h> // getifaddrs

#include <event2/http.h> // evhttp_bind

#include <unistr.h>
#include <uniconv.h>

#include <libavutil/base64.h>

#include "logger.h"
#include "conffile.h"
#include "misc.h"


static char *buildopts[] =
  {
#ifdef HAVE_FFMPEG
    "ffmpeg",
#else
    "libav",
#endif
#ifdef SPOTIFY
    "Spotify",
#else
    "Without Spotify",
#endif
#ifdef SPOTIFY_LIBRESPOTC
    "librespot-c",
#endif
#ifdef LASTFM
    "LastFM",
#else
    "Without LastFM",
#endif
#ifdef CHROMECAST
    "Chromecast",
#else
    "Without Chromecast",
#endif
#ifdef MPD
    "MPD",
#else
    "Without MPD",
#endif
#ifdef HAVE_LIBWEBSOCKETS
    "Websockets",
#else
    "Without websockets",
#endif
#ifdef HAVE_ALSA
    "ALSA",
#else
    "Without ALSA",
#endif
#ifdef HAVE_LIBPULSE
    "Pulseaudio",
#endif
#ifdef WEBINTERFACE
    "Webinterface",
#else
    "Without webinterface",
#endif
#ifdef HAVE_REGEX_H
    "Regex",
#else
    "Without regex",
#endif
    NULL
  };


/* ------------------------ Network utility functions ----------------------- */

static bool
prefix_is_equal(uint8_t *addr1, uint8_t *addr2, uint8_t *mask, size_t len)
{
  int i;

  for (i = 0; i < len; i++)
    {
      if ((addr1[i] & mask[i]) != (addr2[i] & mask[i]))
	return false;
    }

  return true;
}

// Checks if the address is in any of the interface subnets
static bool
net_address_is_local(union net_sockaddr *naddr)
{
  struct ifaddrs *ifaddrs;
  struct ifaddrs *iap;
  void *if_addr;
  void *if_netmask;
  void *addr;
  int addr_len;
  sa_family_t addr_family;
  bool is_local = false;

  // Point addr to the actual address data
  if (naddr->sa.sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&naddr->sin6.sin6_addr))
    {
      addr = naddr->sin6.sin6_addr.s6_addr + 12; // ipv4 over ipv6 prefix is 12 bytes
      addr_len = sizeof(struct in6_addr) - 12;
      addr_family = AF_INET;
    }
  else if (naddr->sa.sa_family == AF_INET6)
    {
      addr = naddr->sin6.sin6_addr.s6_addr;
      addr_len = sizeof(struct in6_addr);
      addr_family = AF_INET6;
    }
  else if (naddr->sa.sa_family == AF_INET)
    {
      addr = &naddr->sin.sin_addr.s_addr;
      addr_len = sizeof(struct in_addr);
      addr_family = AF_INET;
    }
  else
    {
      return false;
    }

  getifaddrs(&ifaddrs);

  for (iap = ifaddrs; iap && !is_local; iap = iap->ifa_next)
    {
      if (!iap->ifa_addr || !iap->ifa_netmask)
	continue;
      if (iap->ifa_addr->sa_family != addr_family || iap->ifa_netmask->sa_family != addr_family)
	continue;

      if (addr_family == AF_INET6)
	{
	  if_addr = ((struct sockaddr_in6 *)iap->ifa_addr)->sin6_addr.s6_addr;
	  if_netmask = ((struct sockaddr_in6 *)iap->ifa_netmask)->sin6_addr.s6_addr;
	}
      else
	{
	  if_addr = &((struct sockaddr_in *)iap->ifa_addr)->sin_addr.s_addr;
	  if_netmask = &((struct sockaddr_in *)iap->ifa_netmask)->sin_addr.s_addr;
	}

      is_local = prefix_is_equal(addr, if_addr, if_netmask, addr_len);
    }

  freeifaddrs(ifaddrs);

  return is_local;
}

static bool
net_address_has_prefix(union net_sockaddr *naddr, const char *prefix)
{
  char buf[64];
  const char *addr = buf;

  if (net_address_get(buf, sizeof(buf), naddr) < 0)
    return false;

  if (naddr->sa.sa_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&naddr->sin6.sin6_addr))
    addr += strlen("::ffff:");

  if (strncmp(addr, prefix, strlen(prefix)) == 0)
    return true;

  return false;
}

bool
net_peer_address_is_trusted(union net_sockaddr *naddr)
{
  cfg_t *section;
  const char *network;
  int i;
  int n;

  if (!naddr)
    return false;

  section = cfg_getsec(cfg, "general");

  n = cfg_size(section, "trusted_networks");
  for (i = 0; i < n; i++)
    {
      network = cfg_getnstr(section, "trusted_networks", i);

      if (!network || network[0] == '\0' || strcmp(network, "none") == 0)
	return false;

      if (strcmp(network, "any") == 0)
	return true;

      if (strcmp(network, "lan") == 0 && net_address_is_local(naddr))
	return true;

      if (strcmp(network, "localhost") == 0)
	network = (naddr->sa.sa_family == AF_INET6) ? "::1" : "127.0.0.1";

      if (net_address_has_prefix(naddr, network))
	return true;
    }

  return false;
}

int
net_address_get(char *addr, size_t addr_len, union net_sockaddr *naddr)
{
  const char *s = NULL;

  memset(addr, 0, addr_len); // Just in case caller doesn't check for errors

  if (naddr->sa.sa_family == AF_INET6)
     s = inet_ntop(AF_INET6, &naddr->sin6.sin6_addr, addr, addr_len);
  else if (naddr->sa.sa_family == AF_INET)
     s = inet_ntop(AF_INET, &naddr->sin.sin_addr, addr, addr_len);

  if (!s)
    return -1;

  return 0;
}

int
net_port_get(short unsigned *port, union net_sockaddr *naddr)
{
  if (naddr->sa.sa_family == AF_INET6)
     *port = ntohs(naddr->sin6.sin6_port);
  else if (naddr->sa.sa_family == AF_INET)
     *port = ntohs(naddr->sin.sin_port);
  else
    return -1;

  return 0;
}

int
net_if_get(char *ifname, size_t ifname_len, const char *addr)
{
  struct ifaddrs *ifaddrs;
  struct ifaddrs *iap;
  char s[64];

  memset(ifname, 0, ifname_len);

  getifaddrs(&ifaddrs);

  for (iap = ifaddrs; iap; iap = iap->ifa_next)
    {
      if (!iap->ifa_addr)
	continue;

      if (net_address_get(s, sizeof(s), (union net_sockaddr *)iap->ifa_addr) < 0)
	continue;

      if (strcmp(s, addr) != 0)
	continue;

      snprintf(ifname, ifname_len, "%s", iap->ifa_name);
      break;
    }

  freeifaddrs(ifaddrs);

  return (ifname[0] != 0) ? 0 : -1;
}

static int
net_connect_impl(const char *addr, unsigned short port, int type, const char *log_service_name, bool set_nonblock)
{
  struct addrinfo hints = { 0 };
  struct addrinfo *servinfo;
  struct addrinfo *ptr;
  char strport[8];
  int flags;
  int fd;
  int ret;

  DPRINTF(E_DBG, L_MISC, "Connecting to '%s' at %s (port %u)\n", log_service_name, addr, port);

  hints.ai_socktype = type;
  hints.ai_family = (cfg_getbool(cfg_getsec(cfg, "general"), "ipv6")) ? AF_UNSPEC : AF_INET;

  snprintf(strport, sizeof(strport), "%hu", port);
  ret = getaddrinfo(addr, strport, &hints, &servinfo);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MISC, "Could not get '%s' address info for %s (port %u): %s\n", log_service_name, addr, port, gai_strerror(ret));
      return -1;
    }

  for (ptr = servinfo; ptr; ptr = ptr->ai_next)
    {
      fd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
      if (fd < 0)
	{
	  continue;
	}

      // For Linux we could just give SOCK_CLOEXEC to socket(), but that won't
      // work with MacOS, so we have to use fcntl()
      flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0)
	continue;
      if (set_nonblock)
	ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK | O_CLOEXEC);
      else
	ret = fcntl(fd, F_SETFL, flags | O_CLOEXEC);
      if (ret < 0)
	continue;

      ret = connect(fd, ptr->ai_addr, ptr->ai_addrlen);
      if (ret < 0 && errno != EINPROGRESS) // EINPROGRESS in case of nonblock
	{
	  close(fd);
	  continue;
	}

      break;
    }

  freeaddrinfo(servinfo);

  if (!ptr)
    {
      DPRINTF(E_LOG, L_MISC, "Could not connect to '%s' at %s (port %u): %s\n", log_service_name, addr, port, strerror(errno));
      return -1;
    }

  // net_address_get(ipaddr, sizeof(ipaddr), (union net_sockaddr *)ptr->ai-addr);

  return fd;
}

int
net_connect(const char *addr, unsigned short port, int type, const char *log_service_name)
{
  return net_connect_impl(addr, port, type, log_service_name, false);
}

// If *port is 0 then a random port will be assigned, and *port will be updated
// with the port number. SOCK_STREAM type services are set to use non-blocking
// sockets.
static int
net_bind_impl(short unsigned *port, int type, const char *log_service_name, bool reuseport)
{
  struct addrinfo hints = { 0 };
  struct addrinfo *servinfo;
  struct addrinfo *ptr;
  union net_sockaddr naddr = { 0 };
  socklen_t naddr_len = sizeof(naddr);
  const char *cfgaddr;
  char addr[INET6_ADDRSTRLEN];
  char strport[8];
  int flags;
  int yes = 1;
  int no = 0;
  int fd = -1;
  int ret;

  cfgaddr = cfg_getstr(cfg_getsec(cfg, "general"), "bind_address");

  hints.ai_socktype = type;
  hints.ai_family = (cfg_getbool(cfg_getsec(cfg, "general"), "ipv6")) ? AF_INET6 : AF_INET;
  hints.ai_flags = cfgaddr ? 0 : AI_PASSIVE;

  snprintf(strport, sizeof(strport), "%hu", *port);
  ret = getaddrinfo(cfgaddr, strport, &hints, &servinfo);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MISC, "Failure creating '%s' service, could not resolve '%s' (port %s): %s\n", log_service_name, cfgaddr ? cfgaddr : "(ANY)", strport, gai_strerror(ret));
      return -1;
    }

  for (ptr = servinfo; ptr != NULL; ptr = ptr->ai_next)
    {
      if (fd >= 0)
	close(fd);

      fd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
      if (fd < 0)
	continue;

      // For Linux we could just give SOCK_NONBLOCK and SOCK_CLOEXEC to
      // socket(), but that won't work with MacOS, so we have to use fcntl()
      flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0)
	continue;
      if (type == SOCK_STREAM)
	ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK | O_CLOEXEC);
      else
	ret = fcntl(fd, F_SETFL, flags | O_CLOEXEC);
      if (ret < 0)
	continue;

      // Makes us able to attach multiple threads to the same port
      if (reuseport)
	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
      else
	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &no, sizeof(no));
      if (ret < 0)
	continue;

      // TODO libevent sets this, we do the same?
      ret = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
      if (ret < 0)
	continue;

      ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      if (ret < 0)
	continue;

      if (ptr->ai_family == AF_INET6)
	{
	  // We want to be sure the service is dual stack
	  ret = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
	  if (ret < 0)
	    continue;
	}

      ret = bind(fd, ptr->ai_addr, ptr->ai_addrlen);
      if (ret < 0)
	continue;

      break;
    }

  freeaddrinfo(servinfo);

  if (!ptr)
    {
      DPRINTF(E_LOG, L_MISC, "Could not create service '%s' with address %s, port %hu: %s\n", log_service_name, cfgaddr ? cfgaddr : "(ANY)", *port, strerror(errno));
      goto error;
    }

  // Get our address (as string) and the port that was assigned (necessary when
  // caller didn't specify a port)
  ret = getsockname(fd, &naddr.sa, &naddr_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MISC, "Error finding address of service '%s': %s\n", log_service_name, strerror(errno));
      goto error;
    }
  else if (naddr_len > sizeof(naddr))
    {
      DPRINTF(E_LOG, L_MISC, "Unexpected address length of service '%s'\n", log_service_name);
      goto error;
    }

  net_port_get(port, &naddr);
  net_address_get(addr, sizeof(addr), &naddr);

  DPRINTF(E_DBG, L_MISC, "Service '%s' bound to %s, port %hu, socket %d\n", log_service_name, addr, *port, fd);

  return fd;

 error:
  if (fd >= 0)
    close(fd);
  return -1;
}

int
net_bind(short unsigned *port, int type, const char *log_service_name)
{
  return net_bind_impl(port, type, log_service_name, false);
}

int
net_bind_with_reuseport(short unsigned *port, int type, const char *log_service_name)
{
  return net_bind_impl(port, type, log_service_name, true);
}

int
net_evhttp_bind(struct evhttp *evhttp, unsigned short port, const char *log_service_name)
{
  const char *bind_address;
  bool v6_enabled;
  int ret;

  bind_address = cfg_getstr(cfg_getsec(cfg, "general"), "bind_address");

  // Normally comply with config, except for "::" where we want to listen on
  // both ipv4 and ipv6 (as the comment in the config file says)
  if (bind_address && strcmp(bind_address, "::") != 0)
    return evhttp_bind_socket(evhttp, bind_address, port);

  // For Linux, we could just do evhttp_bind_socket() for "::", and both the
  // ipv4 and v6 port would be bound. However, for bsd it seems it is necessary
  // to do like below.
  v6_enabled = cfg_getbool(cfg_getsec(cfg, "general"), "ipv6");
  if (v6_enabled)
    {
      ret = evhttp_bind_socket(evhttp, "::", port);
      if (ret < 0)
	{
	  DPRINTF(E_LOG, L_MISC, "Could not bind service '%s' to port %d with IPv6, falling back to IPv4\n", log_service_name, port);
	  v6_enabled = 0;
	}
    }

  ret = evhttp_bind_socket(evhttp, "0.0.0.0", port);
  if (ret < 0)
    {
      if (!v6_enabled)
	return -1;

#ifndef __linux__
      DPRINTF(E_LOG, L_MISC, "Could not bind service '%s' to port %d with IPv4, listening on IPv6 only\n", log_service_name, port);
#endif
    }

  return 0;
}

bool
net_is_http_or_https(const char *url)
{
  return (strncasecmp(url, "http://", strlen("http://")) == 0 || strncasecmp(url, "https://", strlen("https://")) == 0);
}

/* ----------------------- Conversion/hashing/sanitizers -------------------- */

int
safe_atoi32(const char *str, int32_t *val)
{
  char *end;
  long intval;

  if (str == NULL)
    {
      DPRINTF(E_SPAM, L_MISC, "Input to safe_atoi32 is NULL\n");
      return -1;
    }

  errno = 0;
  intval = strtol(str, &end, 10);

  if (((errno == ERANGE) && ((intval == LONG_MAX) || (intval == LONG_MIN)))
      || ((errno != 0) && (intval == 0)))
    {
      DPRINTF(E_DBG, L_MISC, "Invalid i32 in string (%s): %s\n", str, strerror(errno));
      return -1;
    }

  if (end == str)
    {
      DPRINTF(E_DBG, L_MISC, "No i32 found in string (%s)\n", str);
      return -1;
    }

  if (intval > INT32_MAX)
    {
      DPRINTF(E_DBG, L_MISC, "i32 value out of range (%s)\n", str);
      return -1;
    }

  *val = (int32_t)intval;

  return 0;
}

int
safe_atou32(const char *str, uint32_t *val)
{
  char *end;
  unsigned long intval;

  if (str == NULL)
    {
      DPRINTF(E_SPAM, L_MISC, "Input to safe_atou32 is NULL\n");
      return -1;
    }

  errno = 0;
  intval = strtoul(str, &end, 10);

  if (((errno == ERANGE) && (intval == ULONG_MAX))
      || ((errno != 0) && (intval == 0)))
    {
      DPRINTF(E_DBG, L_MISC, "Invalid u32 in string (%s): %s\n", str, strerror(errno));
      return -1;
    }

  if (end == str)
    {
      DPRINTF(E_DBG, L_MISC, "No u32 found in string (%s)\n", str);
      return -1;
    }

  if (intval > UINT32_MAX)
    {
      DPRINTF(E_DBG, L_MISC, "u32 value out of range (%s)\n", str);
      return -1;
    }

  *val = (uint32_t)intval;

  return 0;
}

int
safe_hextou32(const char *str, uint32_t *val)
{
  char *end;
  unsigned long intval;

  if (str == NULL)
    {
      DPRINTF(E_SPAM, L_MISC, "Input to safe_hextou32 is NULL\n");
      return -1;
    }

  errno = 0;
  intval = strtoul(str, &end, 16);

  if (((errno == ERANGE) && (intval == ULONG_MAX))
      || ((errno != 0) && (intval == 0)))
    {
      DPRINTF(E_DBG, L_MISC, "Invalid u32 in string (%s): %s\n", str, strerror(errno));
      return -1;
    }

  if (end == str)
    {
      DPRINTF(E_DBG, L_MISC, "No u32 found in string (%s)\n", str);
      return -1;
    }

  if (intval > UINT32_MAX)
    {
      DPRINTF(E_DBG, L_MISC, "u32 value out of range (%s)\n", str);
      return -1;
    }

  *val = (uint32_t)intval;

  return 0;
}

int
safe_atoi64(const char *str, int64_t *val)
{
  char *end;
  long long intval;

  if (str == NULL)
    {
      DPRINTF(E_SPAM, L_MISC, "Input to safe_atoi64 is NULL\n");
      return -1;
    }

  errno = 0;
  intval = strtoll(str, &end, 10);

  if (((errno == ERANGE) && ((intval == LLONG_MAX) || (intval == LLONG_MIN)))
      || ((errno != 0) && (intval == 0)))
    {
      DPRINTF(E_DBG, L_MISC, "Invalid i64 in string (%s): %s\n", str, strerror(errno));
      return -1;
    }

  if (end == str)
    {
      DPRINTF(E_DBG, L_MISC, "No i64 found in string (%s)\n", str);
      return -1;
    }

  if (intval > INT64_MAX)
    {
      DPRINTF(E_DBG, L_MISC, "i64 value out of range (%s)\n", str);
      return -1;
    }

  *val = (int64_t)intval;

  return 0;
}

int
safe_atou64(const char *str, uint64_t *val)
{
  char *end;
  unsigned long long intval;

  if (str == NULL)
    {
      DPRINTF(E_SPAM, L_MISC, "Input to safe_atou64 is NULL\n");
      return -1;
    }

  errno = 0;
  intval = strtoull(str, &end, 10);

  if (((errno == ERANGE) && (intval == ULLONG_MAX))
      || ((errno != 0) && (intval == 0)))
    {
      DPRINTF(E_DBG, L_MISC, "Invalid u64 in string (%s): %s\n", str, strerror(errno));

      return -1;
    }

  if (end == str)
    {
      DPRINTF(E_DBG, L_MISC, "No u64 found in string (%s)\n", str);

      return -1;
    }

  if (intval > UINT64_MAX)
    {
      DPRINTF(E_DBG, L_MISC, "u64 value out of range (%s)\n", str);
      return -1;
    }

  *val = (uint64_t)intval;

  return 0;
}

int
safe_hextou64(const char *str, uint64_t *val)
{
  char *end;
  unsigned long long intval;

  if (str == NULL)
    {
      DPRINTF(E_SPAM, L_MISC, "Input to safe_hextou64 is NULL\n");
      return -1;
    }

  errno = 0;
  intval = strtoull(str, &end, 16);

  if (((errno == ERANGE) && (intval == ULLONG_MAX))
      || ((errno != 0) && (intval == 0)))
    {
      DPRINTF(E_DBG, L_MISC, "Invalid u64 in string (%s): %s\n", str, strerror(errno));
      return -1;
    }

  if (end == str)
    {
      DPRINTF(E_DBG, L_MISC, "No u64 found in string (%s)\n", str);
      return -1;
    }

  if (intval > UINT64_MAX)
    {
      DPRINTF(E_DBG, L_MISC, "u64 value out of range (%s)\n", str);
      return -1;
    }

  *val = (uint64_t)intval;

  return 0;
}

char *
safe_strdup(const char *str)
{
  if (str == NULL)
    return NULL;

  return strdup(str);
}

/*
 * Wrapper function for vasprintf by Intel Corporation
 * Published under the L-GPL 2.1 licence as part of clr-boot-manager
 *
 * https://github.com/clearlinux/clr-boot-manager
 */
char *
safe_asprintf(const char *fmt, ...)
{
  char *ret = NULL;
  va_list va;

  va_start(va, fmt);
  if (vasprintf(&ret, fmt, va) < 0)
    {
      DPRINTF(E_FATAL, L_MISC, "Out of memory for safe_asprintf\n");
      abort();
    }
  va_end(va);

  return ret;
}

int
safe_snprintf_cat(char *dst, size_t n, const char *fmt, ...)
{
  size_t dstlen;
  va_list va;
  int ret;

  if (!dst || !fmt)
    return -1;

  dstlen = strlen(dst);
  if (n < dstlen)
    return -1;

  va_start(va, fmt);
  ret = vsnprintf(dst + dstlen, n - dstlen, fmt, va);
  va_end(va);

  if (ret >= 0 && ret < n - dstlen)
    return 0;
  else
    return -1;
}

int
safe_snreplace(char *s, size_t sz, const char *pattern, const char *replacement)
{
  char *ptr;
  char *src;
  char *dst;
  size_t num;

  if (!s)
    return -1;

  if (!pattern || !replacement)
    return 0;

  size_t p_len = strlen(pattern);
  size_t r_len = strlen(replacement);
  size_t s_len = strlen(s) + 1; // Incl terminator

  ptr = s;
  while ((ptr = strstr(ptr, pattern)))
    {
      // We will move the part of the string after the pattern from src to dst
      src = ptr + p_len;
      dst = ptr + r_len;

      num = s_len - (src - s); // Number of bytes w/terminator we need to move
      if (dst + num > s + sz)
	return -1; // Not enough room

      // Move everything after the pattern, use memmove since there might be an overlap
      memmove(dst, src, num);

      // Write replacement, no null terminater
      memcpy(ptr, replacement, r_len);

      // String now has a new length
      s_len += r_len - p_len;

      // Advance ptr to avoid infinite looping
      ptr = dst;
    }

  return 0;
}


char *
unicode_fixup_string(char *str, const char *fromcode)
{
  uint8_t *ret;
  size_t len;

  if (!str)
    return NULL;

  len = strlen(str);

  /* String is valid UTF-8 */
  if (!u8_check((uint8_t *)str, len))
    {
      if (len >= 3)
	{
	  /* Check for and strip byte-order mark */
	  if (memcmp("\xef\xbb\xbf", str, 3) == 0)
	    memmove(str, str + 3, len - 3 + 1);
	}

      return str;
    }

  ret = u8_strconv_from_encoding(str, fromcode, iconveh_question_mark);
  if (!ret)
    {
      DPRINTF(E_LOG, L_MISC, "Could not convert string '%s' to UTF-8: %s\n", str, strerror(errno));

      return NULL;
    }

  return (char *)ret;
}

char *
trim(char *str)
{
  size_t start; // Position of first non-space char
  size_t term;  // Position of 0-terminator

  if (!str)
    return NULL;

  start = 0;
  term  = strlen(str);

  while ((start < term) && isspace(str[start]))
    start++;
  while ((term > start) && isspace(str[term - 1]))
    term--;

  str[term] = '\0';

  // Shift chars incl. terminator
  if (start)
    memmove(str, str + start, term - start + 1);

  return str;
}

char *
atrim(const char *str)
{
  size_t start; // Position of first non-space char
  size_t term;  // Position of 0-terminator
  size_t size;
  char *result;

  if (!str)
    return NULL;

  start = 0;
  term  = strlen(str);

  while ((start < term) && isspace(str[start]))
    start++;
  while ((term > start) && isspace(str[term - 1]))
    term--;

  size = term - start + 1;

  result = malloc(size);

  memcpy(result, str + start, size);
  result[size - 1] = '\0';

  return result;
}

void
swap_pointers(char **a, char **b)
{
  char *t = *a;
  *a = *b;
  *b = t;
}

uint32_t
djb_hash(const void *data, size_t len)
{
  const unsigned char *bytes = data;
  uint32_t hash = 5381;

  while (len--)
    {
      hash = ((hash << 5) + hash) + *bytes;
      bytes++;
    }

  return hash;
}

int64_t
two_str_hash(const char *a, const char *b)
{
  char hashbuf[2048];
  int64_t hash;
  int i;
  int ret;

  ret = snprintf(hashbuf, sizeof(hashbuf), "%s==%s", (a) ? a : "", (b) ? b : "");
  if (ret < 0 || ret == sizeof(hashbuf))
    {
      DPRINTF(E_LOG, L_MISC, "Buffer too large to calculate hash: '%s==%s'\n", a, b);
      return 999999; // Stand-in hash...
    }

  for (i = 0; hashbuf[i]; i++)
    hashbuf[i] = tolower(hashbuf[i]);

  // Limit hash length to 63 bits, due to signed type in sqlite
  hash = murmur_hash64(hashbuf, strlen(hashbuf), 0) >> 1;

  return hash;
}

uint8_t *
b64_decode(int *dstlen, const char *src)
{
  uint8_t *out;
  int len;
  int ret;

  len = AV_BASE64_DECODE_SIZE(strlen(src));

  // Add a extra zero byte just in case we are decoding a string without null
  // termination
  CHECK_NULL(L_MISC, out = calloc(1, len + 1));

  ret = av_base64_decode(out, src, len);
  if (ret < 0)
    {
      free(out);
      return NULL;
    }

  if (dstlen)
    *dstlen = ret;

  return out;
}

char *
b64_encode(const uint8_t *src, int srclen)
{
  char *out;
  int len;
  char *ret;

  len = AV_BASE64_SIZE(srclen);

  CHECK_NULL(L_MISC, out = calloc(1, len));

  ret = av_base64_encode(out, len, src, srclen);
  if (!ret)
    {
      free(out);
      return NULL;
    }

  return out;
}


/*
 * MurmurHash2, 64-bit versions, by Austin Appleby
 *
 * Code released under the public domain, as per
 *    <http://murmurhash.googlepages.com/>
 * as of 2010-01-03.
 */

#if SIZEOF_VOID_P == 8 /* 64bit platforms */

uint64_t
murmur_hash64(const void *key, int len, uint32_t seed)
{
  const int r = 47;
  const uint64_t m = 0xc6a4a7935bd1e995;

  const uint64_t *data;
  const uint64_t *end;
  const unsigned char *data_tail;
  uint64_t h;
  uint64_t k;

  h = seed ^ (len * m);
  data = (const uint64_t *)key;
  end = data + (len / 8);

  while (data != end)
    {
      k = *data++;

      k *= m;
      k ^= k >> r;
      k *= m;

      h ^= k;
      h *= m;
    }

  data_tail = (const unsigned char *)data;

  switch (len & 7)
    {
      case 7:
	h ^= (uint64_t)(data_tail[6]) << 48; /* FALLTHROUGH */
      case 6:
	h ^= (uint64_t)(data_tail[5]) << 40; /* FALLTHROUGH */
      case 5:
	h ^= (uint64_t)(data_tail[4]) << 32; /* FALLTHROUGH */
      case 4:
	h ^= (uint64_t)(data_tail[3]) << 24; /* FALLTHROUGH */
      case 3:
	h ^= (uint64_t)(data_tail[2]) << 16; /* FALLTHROUGH */
      case 2:
	h ^= (uint64_t)(data_tail[1]) << 8; /* FALLTHROUGH */
      case 1:
	h ^= (uint64_t)(data_tail[0]);
	h *= m;
    }

  h ^= h >> r;
  h *= m;
  h ^= h >> r;

  return h;
}

#elif SIZEOF_VOID_P == 4 /* 32bit platforms */

uint64_t
murmur_hash64(const void *key, int len, uint32_t seed)
{
  const int r = 24;
  const uint32_t m = 0x5bd1e995;

  const uint32_t *data;
  const unsigned char *data_tail;
  uint32_t k1;
  uint32_t h1;
  uint32_t k2;
  uint32_t h2;

  uint64_t h;

  h1 = seed ^ len;
  h2 = 0;

  data = (const uint32_t *)key;

  while (len >= 8)
    {
      k1 = *data++;
      k1 *= m; k1 ^= k1 >> r; k1 *= m;
      h1 *= m; h1 ^= k1;

      k2 = *data++;
      k2 *= m; k2 ^= k2 >> r; k2 *= m;
      h2 *= m; h2 ^= k2;

      len -= 8;
    }

  if (len >= 4)
    {
      k1 = *data++;
      k1 *= m; k1 ^= k1 >> r; k1 *= m;
      h1 *= m; h1 ^= k1;
      len -= 4;
    }

  data_tail = (const unsigned char *)data;

  switch(len)
    {
      case 3:
	h2 ^= (uint32_t)(data_tail[2]) << 16;
      case 2:
	h2 ^= (uint32_t)(data_tail[1]) << 8;
      case 1:
	h2 ^= (uint32_t)(data_tail[0]);
	h2 *= m;
    };

  h1 ^= h2 >> 18; h1 *= m;
  h2 ^= h1 >> 22; h2 *= m;
  h1 ^= h2 >> 17; h1 *= m;
  h2 ^= h1 >> 19; h2 *= m;

  h = h1;
  h = (h << 32) | h2;

  return h;
}
#else
# error Platform not supported
#endif


/* --------------------------- Key/value functions -------------------------- */

struct keyval *
keyval_alloc(void)
{
  struct keyval *kv;

  kv = calloc(1, sizeof(struct keyval));
  if (!kv)
    {
      DPRINTF(E_LOG, L_MISC, "Out of memory for keyval alloc\n");

      return NULL;
    }

  return kv;
}

int
keyval_add_size(struct keyval *kv, const char *name, const char *value, size_t size)
{
  struct onekeyval *okv;
  const char *val;

  if (!kv)
    return -1;

  /* Check for duplicate key names */
  val = keyval_get(kv, name);
  if (val)
    {
      /* Same value, fine */
      if (strcmp(val, value) == 0)
        return 0;
      else /* Different value, bad */
        return -1;
    }

  okv = (struct onekeyval *)malloc(sizeof(struct onekeyval));
  if (!okv)
    {
      DPRINTF(E_LOG, L_MISC, "Out of memory for new keyval\n");

      return -1;
    }

  okv->name = strdup(name);
  if (!okv->name)
    {
      DPRINTF(E_LOG, L_MISC, "Out of memory for new keyval name\n");

      free(okv);
      return -1;
    }

  okv->value = (char *)malloc(size + 1);
  if (!okv->value)
    {
      DPRINTF(E_LOG, L_MISC, "Out of memory for new keyval value\n");

      free(okv->name);
      free(okv);
      return -1;
    }

  memcpy(okv->value, value, size);
  okv->value[size] = '\0';

  okv->next = NULL;

  if (!kv->head)
    kv->head = okv;

  if (kv->tail)
    kv->tail->next = okv;

  kv->tail = okv;

  return 0;
}

int
keyval_add(struct keyval *kv, const char *name, const char *value)
{
  return keyval_add_size(kv, name, value, strlen(value));
}

void
keyval_remove(struct keyval *kv, const char *name)
{
  struct onekeyval *okv;
  struct onekeyval *pokv;

  if (!kv)
    return;

  for (pokv = NULL, okv = kv->head; okv; pokv = okv, okv = okv->next)
    {
      if (strcasecmp(okv->name, name) == 0)
        break;
    }

  if (!okv)
    return;

  if (okv == kv->head)
    kv->head = okv->next;

  if (okv == kv->tail)
    kv->tail = pokv;

  if (pokv)
    pokv->next = okv->next;

  free(okv->name);
  free(okv->value);
  free(okv);
}

const char *
keyval_get(struct keyval *kv, const char *name)
{
  struct onekeyval *okv;

  if (!kv)
    return NULL;

  for (okv = kv->head; okv; okv = okv->next)
    {
      if (strcasecmp(okv->name, name) == 0)
        return okv->value;
    }

  return NULL;
}

void
keyval_clear(struct keyval *kv)
{
  struct onekeyval *hokv;
  struct onekeyval *okv;

  if (!kv)
    return;

  hokv = kv->head;

  for (okv = hokv; hokv; okv = hokv)
    {
      hokv = okv->next;

      free(okv->name);
      free(okv->value);
      free(okv);
    }

  kv->head = NULL;
  kv->tail = NULL;
}

void
keyval_sort(struct keyval *kv)
{
  struct onekeyval *head;
  struct onekeyval *okv;
  struct onekeyval *sokv;

  if (!kv || !kv->head)
    return;

  head = kv->head;
  for (okv = kv->head; okv; okv = okv->next)
    {
      okv->sort = NULL;
      for (sokv = kv->head; sokv; sokv = sokv->next)
	{
	  // We try to find a name which is greater than okv->name
	  // but less than our current candidate (okv->sort->name)
	  if ( (strcmp(sokv->name, okv->name) > 0) &&
	       ((okv->sort == NULL) || (strcmp(sokv->name, okv->sort->name) < 0)) )
	    okv->sort = sokv;
	}

      // Find smallest name, which will be the new head
      if (strcmp(okv->name, head->name) < 0)
	head = okv;
    }

  while ((okv = kv->head))
    {
      kv->head  = okv->next;
      okv->next = okv->sort;
    }

  kv->head = head;
  for (okv = kv->head; okv; okv = okv->next)
    kv->tail = okv;

  DPRINTF(E_DBG, L_MISC, "Keyval sorted. New head: %s. New tail: %s.\n", kv->head->name, kv->tail->name);
}


/* ------------------------------- Ringbuffer ------------------------------- */

int
ringbuffer_init(struct ringbuffer *buf, size_t size)
{
  memset(buf, 0, sizeof(struct ringbuffer));

  CHECK_NULL(L_MISC, buf->buffer = malloc(size));
  buf->size = size;
  buf->write_avail = size;
  return 0;
}

void
ringbuffer_free(struct ringbuffer *buf, bool content_only)
{
  if (!buf)
    return;

  free(buf->buffer);

  if (content_only)
    memset(buf, 0, sizeof(struct ringbuffer));
  else
    free(buf);
}

size_t
ringbuffer_write(struct ringbuffer *buf, const void* src, size_t srclen)
{
  int remaining;

  if (buf->write_avail == 0 || srclen == 0)
    return 0;

  if (srclen > buf->write_avail)
   srclen = buf->write_avail;

  remaining = buf->size - buf->write_pos;
  if (srclen > remaining)
    {
      memcpy(buf->buffer + buf->write_pos, src, remaining);
      memcpy(buf->buffer, src + remaining, srclen - remaining);
    }
  else
    {
      memcpy(buf->buffer + buf->write_pos, src, srclen);
    }

  buf->write_pos = (buf->write_pos + srclen) % buf->size;

  buf->write_avail -= srclen;
  buf->read_avail += srclen;

  return srclen;
}

size_t
ringbuffer_read(uint8_t **dst, size_t dstlen, struct ringbuffer *buf)
{
  int remaining;

  *dst = buf->buffer + buf->read_pos;

  if (buf->read_avail == 0 || dstlen == 0)
    return 0;

  remaining = buf->size - buf->read_pos;

  // The number of bytes we will return will be MIN(dstlen, remaining, read_avail)
  if (dstlen > remaining)
    dstlen = remaining;
  if (dstlen > buf->read_avail)
    dstlen = buf->read_avail;

  buf->read_pos = (buf->read_pos + dstlen) % buf->size;

  buf->write_avail += dstlen;
  buf->read_avail -= dstlen;

  return dstlen;
}


/* ------------------------- Clock utility functions ------------------------ */

int
clock_gettime_with_res(clockid_t clock_id, struct timespec *tp, struct timespec *res)
{
  int ret;

  if ((!tp) || (!res))
    return -1;

  ret = clock_gettime(clock_id, tp);
  /* this will only work for sub-second resolutions. */
  if (ret == 0 && res->tv_nsec > 1)
    tp->tv_nsec = (tp->tv_nsec/res->tv_nsec)*res->tv_nsec;

  return ret;
}

struct timespec
timespec_add(struct timespec time1, struct timespec time2)
{
  struct timespec result;

  result.tv_sec = time1.tv_sec + time2.tv_sec;
  result.tv_nsec = time1.tv_nsec + time2.tv_nsec;
  if (result.tv_nsec >= 1000000000L)
    {
      result.tv_sec++;
      result.tv_nsec -= 1000000000L;
    }
  return result;
}

int
timespec_cmp(struct timespec time1, struct timespec time2)
{
  /* Less than. */
  if (time1.tv_sec < time2.tv_sec)
    return -1;
  /* Greater than. */
  else if (time1.tv_sec > time2.tv_sec)
    return 1;
  /* Less than. */
  else if (time1.tv_nsec < time2.tv_nsec)
    return -1;
  /* Greater than. */
  else if (time1.tv_nsec > time2.tv_nsec)
    return 1;
  /* Equal. */
  else
    return 0;
}

struct timespec
timespec_reltoabs(struct timespec relative)
{
  struct timespec absolute;

#ifdef CLOCK_REALTIME
  clock_gettime(CLOCK_REALTIME, &absolute);
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  TIMEVAL_TO_TIMESPEC(&tv, &absolute);
#endif
  return timespec_add(absolute, relative);
}

#if defined(HAVE_MACH_CLOCK) || defined(HAVE_MACH_TIMER)

#include <mach/mach_time.h> /* mach_absolute_time */
#include <mach/mach.h>      /* host_get_clock_service */
#include <mach/clock.h>     /* clock_get_time */

/* mach monotonic clock port */
extern mach_port_t clock_port;

#ifndef HAVE_CLOCK_GETTIME

int
clock_gettime(clockid_t clock_id, struct timespec *tp)
{
  static int clock_init = 0;
  static clock_serv_t clock;

  mach_timespec_t mts;
  int ret;

  if (! clock_init) {
    clock_init = 1;
    if (host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &clock))
      abort(); /* unlikely */
  }

  if(! tp)
    return -1;

  switch (clock_id) {

  case CLOCK_REALTIME:

    /* query mach for calendar time */
    ret = clock_get_time(clock, &mts);
    if (! ret) {
      tp->tv_sec = mts.tv_sec;
      tp->tv_nsec = mts.tv_nsec;
    }
    break;

  case CLOCK_MONOTONIC:

    /* query mach for monotinic time */
    ret = clock_get_time(clock_port, &mts);
    if (! ret) {
      tp->tv_sec = mts.tv_sec;
      tp->tv_nsec = mts.tv_nsec;
    }
    break;

  default:
    ret = -1;
    break;
  }

  return ret;
}

int
clock_getres(clockid_t clock_id, struct timespec *res)
{
  if (! res)
    return -1;

  /* hardcode ms resolution */
  res->tv_sec = 0;
  res->tv_nsec = 1000;

  return 0;
}

#endif /* HAVE_CLOCK_GETTIME */

#ifndef HAVE_TIMER_SETTIME

#include <sys/time.h> /* ITIMER_REAL */

int
timer_create(clockid_t clock_id, void *sevp, timer_t *timer_id)
{
  if (clock_id != CLOCK_MONOTONIC)
    return -1;
  if (sevp)
    return -1;

  /* setitimer only supports one timer */
  *timer_id = 0;

  return 0;
}

int
timer_delete(timer_t timer_id)
{
  struct itimerval timerval;

  if (timer_id != 0)
    return -1;

  memset(&timerval, 0, sizeof(struct itimerval));

  return setitimer(ITIMER_REAL, &timerval, NULL);
}

int
timer_settime(timer_t timer_id, int flags, const struct itimerspec *tp, struct itimerspec *old)
{
  struct itimerval tv;

  if (timer_id != 0 || ! tp || old)
    return -1;

  TIMESPEC_TO_TIMEVAL(&(tv.it_value), &(tp->it_value));
  TIMESPEC_TO_TIMEVAL(&(tv.it_interval), &(tp->it_interval));

  return setitimer(ITIMER_REAL, &tv, NULL);
}

int
timer_getoverrun(timer_t timer_id)
{
  /* since we don't know if there have been signals that weren't delivered,
     assume none */
  return 0;
}

#endif /* HAVE_TIMER_SETTIME */

#endif /* HAVE_MACH_CLOCK */


/* ------------------------------- Media quality ---------------------------- */

bool
quality_is_equal(struct media_quality *a, struct media_quality *b)
{
  return (a->sample_rate == b->sample_rate && a->bits_per_sample == b->bits_per_sample && a->channels == b->channels && a->bit_rate == b->bit_rate);
}

enum media_format
media_format_from_string(const char *s)
{
  if (strcmp(s, "pcm") == 0)
    return MEDIA_FORMAT_PCM;
  if (strcmp(s, "wav") == 0)
    return MEDIA_FORMAT_WAV;
  if (strcmp(s, "mp3") == 0)
    return MEDIA_FORMAT_MP3;
  if (strcmp(s, "alac") == 0)
    return MEDIA_FORMAT_ALAC;
  if (strcmp(s, "opus") == 0)
    return MEDIA_FORMAT_OPUS;

  return MEDIA_FORMAT_UNKNOWN;
}

const char *
media_format_to_string(enum media_format format)
{
  if (format == MEDIA_FORMAT_PCM)
    return "pcm";
  if (format == MEDIA_FORMAT_WAV)
    return "wav";
  if (format == MEDIA_FORMAT_MP3)
    return "mp3";
  if (format == MEDIA_FORMAT_ALAC)
    return "alac";
  if (format == MEDIA_FORMAT_OPUS)
    return "opus";

  return "unknown";
}


/* -------------------------- Misc utility functions ------------------------ */

char **
buildopts_get()
{
  return buildopts;
}

int
mutex_init(pthread_mutex_t *mutex)
{
  pthread_mutexattr_t mattr;
  int err;

  CHECK_ERR(L_MISC, pthread_mutexattr_init(&mattr));
  CHECK_ERR(L_MISC, pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK));
  err = pthread_mutex_init(mutex, &mattr);
  CHECK_ERR(L_MISC, pthread_mutexattr_destroy(&mattr));

  return err;
}

void
thread_setname(pthread_t thread, const char *name)
{
#if defined(HAVE_PTHREAD_SETNAME_NP)
  pthread_setname_np(thread, name);
#elif defined(HAVE_PTHREAD_SET_NAME_NP)
  pthread_set_name_np(thread, name);
#endif
}

#ifdef HAVE_UUID
void
uuid_make(char *str)
{
  uuid_t uu;

  uuid_generate_random(uu);
  uuid_unparse_upper(uu, str);
}
#else
void
uuid_make(char *str)
{
  uint16_t uuid[8];
  time_t now;
  int i;

  now = time(NULL);

  srand((unsigned int)now);

  for (i = 0; i < ARRAY_SIZE(uuid); i++)
    {
      uuid[i] = (uint16_t)rand();

      // time_hi_and_version, set version to 4 (=random)
      if (i == 3)
	uuid[i] = (uuid[i] & 0x0FFF) | 0x4000;
      // clock_seq, variant 1
      if (i == 4)
	uuid[i] = (uuid[i] & 0x3FFF) | 0x8000;


      if (i == 2 || i == 3 || i == 4 || i == 5)
	str += sprintf(str, "-");

      str += sprintf(str, "%04" PRIX16, uuid[i]);
    }
}
#endif

int
linear_regression(double *m, double *b, double *r2, const double *x, const double *y, int n)
{
  double x_val;
  double sum_x  = 0;
  double sum_x2 = 0;
  double sum_y  = 0;
  double sum_y2 = 0;
  double sum_xy = 0;
  double denom;
  int i;

  for (i = 0; i < n; i++)
    {
      x_val   = x ? x[i] : (double)i;
      sum_x  += x_val;
      sum_x2 += x_val * x_val;
      sum_y  += y[i];
      sum_y2 += y[i] * y[i];
      sum_xy += x_val * y[i];
    }

  denom = (n * sum_x2 - sum_x * sum_x);
  if (denom == 0)
    return -1;

  *m = (n * sum_xy - sum_x * sum_y) / denom;
  *b = (sum_y * sum_x2 - sum_x * sum_xy) / denom;
  if (r2)
    *r2 = (sum_xy - (sum_x * sum_y)/n) * (sum_xy - (sum_x * sum_y)/n) / ((sum_x2 - (sum_x * sum_x)/n) * (sum_y2 - (sum_y * sum_y)/n));

  return 0;
}

char **
m_readfile(const char *path, int num_lines)
{
  char buf[256];
  FILE *fp;
  char **lines;
  char *line;
  int i;

  // Alloc array of char pointers
  lines = calloc(num_lines, sizeof(char *));
  if (!lines)
    return NULL;

  fp = fopen(path, "rb");
  if (!fp)
    {
      DPRINTF(E_LOG, L_MISC, "Could not open file '%s' for reading: %s\n", path, strerror(errno));
      free(lines);
      return NULL;
    }

  for (i = 0; i < num_lines; i++)
    {
      line = fgets(buf, sizeof(buf), fp);
      if (!line)
	{
	  DPRINTF(E_LOG, L_MISC, "File '%s' has fewer lines than expected (found %d, expected %d)\n", path, i, num_lines);
	  goto error;
	}

      lines[i] = atrim(line);
      if (!lines[i] || (strlen(lines[i]) == 0))
	{
	  DPRINTF(E_LOG, L_MISC, "Line %d in '%s' is invalid\n", i+1, path);
	  goto error;
	}
    }

  fclose(fp);

  return lines;

 error:
  for (i = 0; i < num_lines; i++)
    free(lines[i]);

  free(lines);
  fclose(fp);
  return NULL;
}


/* -------------------------------- Assertion ------------------------------- */

void
log_fatal_err(int domain, const char *func, int line, int err)
{
  DPRINTF(E_FATAL, domain, "%s failed at line %d, error %d (%s)\n", func, line, err, strerror(err));
  abort();
}

void
log_fatal_errno(int domain, const char *func, int line)
{
  DPRINTF(E_FATAL, domain, "%s failed at line %d, error %d (%s)\n", func, line, errno, strerror(errno));
  abort();
}

void
log_fatal_null(int domain, const char *func, int line)
{
  DPRINTF(E_FATAL, domain, "%s returned NULL at line %d\n", func, line);
  abort();
}


