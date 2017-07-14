#!/bin/bash

if [ -n "$*" ] ; then
  exec $*
fi

/usr/sbin/syslogd -n &
/usr/sbin/postfix start

function cleanup {
  echo "received signal stopping..."
  /usr/sbin/postfix stop
  exit
}

trap cleanup SIGHUP SIGINT SIGTERM

tail -f /dev/null
cleanup

