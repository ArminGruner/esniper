AC_INIT(esniper,2.35.1)
AC_CONFIG_SRCDIR(esniper.c)
AM_INIT_AUTOMAKE

AC_PROG_CC
AC_PROG_MAKE_SET

AC_ARG_WITH(curl-config, [  --with-curl-config=PATH Location of libcurl curl-config []], curl_config="$withval", curl_config="curl-config")

if test -f $curl_config ; then
  CURL_CONFIG=$curl_config
  AC_MSG_RESULT(yes)
else				dnl check in path
  AC_CHECK_PROGS(CURL_CONFIG, curl-config)
fi

if test "X$CURL_CONFIG" = "X"; then
  AC_MSG_ERROR($curl_config not found.
cURL is available from http://curl.haxx.se/)
fi

CURLCFLAGS="`$CURL_CONFIG --cflags`"
CURLLIBS=`$CURL_CONFIG --libs`
AC_SUBST(CURLCFLAGS)
AC_SUBST(CURLLIBS)
CFLAGS="$CURLCFLAGS $CFLAGS"
LIBS="$CURLLIBS $LIBS"

dnl check version curl_easy were added in 7.1.1
check_min=7.1.1

AC_CHECK_FUNC(curl_easy_setopt, 
		   [],
		   AC_MSG_ERROR([cURL $check_min or newer required \
(cURL is available from http://curl.haxx.se/)]))

AC_CHECK_FUNC(curl_easy_strerror, 
		   [],
		   CURLCFLAGS="$CURLCFLAGS -D NEED_CURL_EASY_STRERROR")

dnl check for curl SSL support
AC_MSG_CHECKING(for cURL SSL support)
if test XSSL != X`$CURL_CONFIG --feature|grep SSL`; then
  AC_MSG_ERROR([no SSL
cURL is available from http://curl.haxx.se/])
else
  AC_MSG_RESULT(yes)
fi

AC_OUTPUT(Makefile)
