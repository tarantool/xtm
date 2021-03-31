#ifndef TARANTOOL_XTM_CONFIG_H_INCLUDED
#define TARANTOOL_XTM_CONFIG_H_INCLUDED

/*
 * Defined if this platform has madvise(..)
 * and flags we're interested in.
 */
#cmakedefine TARANTOOL_XTM_HAVE_EVENTFD 1

#if defined(TARANTOOL_XTM_HAVE_EVENTFD)
# define TARANTOOL_XTM_USE_EVENTFD 1
#endif

#endif /* TARANTOOL_XTM_CONFIG_H_INCLUDED */
