#! /bin/sh

major=`grep RAR2FS_MAJOR_VER $1 | cut -d' ' -f5`
minor=`grep RAR2FS_MINOR $1 | cut -d' ' -f5`
patch=`grep RAR2FS_PATCH_LVL $1 | cut -d' ' -f5`

e=`which echo`
test ! -z $e && \
	$e -n "$major.$minor.$patch" || \
	echo -n "$major.$minor.$patch"

