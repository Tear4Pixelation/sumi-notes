#include $(call all-subdir-makefiles)

# Note that symlinking source dirs is a terrible idea which can create a huge mess when trying to open files,
#  esp. when debugging
# Resolve the repo root from this file's own location (jni -> main -> src -> app -> android ->
# syncscribble -> repo root) rather than hardcoding an absolute path, so the build works from any
# checkout.  Captured into a simply expanded variable because $(call my-dir) tracks the makefile
# currently being read, and so changes as soon as we include the first one below.
WRITE_ROOT := $(call my-dir)/../../../../../..
include $(WRITE_ROOT)/SDL/Android.mk
include $(WRITE_ROOT)/syncscribble/Makefile
