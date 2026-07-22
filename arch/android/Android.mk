# Copyright (C) 2009 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

JENT_ROOT := $(LOCAL_PATH)/../..

LOCAL_MODULE := jitterentropy

# The entropy collection core must not be optimized (see the __OPTIMIZE__
# guard in src/jitterentropy-base.c). Bionic ships pthreads inside libc, so
# the internal timer needs no extra link library on Android.
LOCAL_CFLAGS := -O0 -DJENT_CONF_ENABLE_INTERNAL_TIMER

LOCAL_C_INCLUDES := $(JENT_ROOT) $(JENT_ROOT)/src $(JENT_ROOT)/arch

# The library consists of all core sources in src/ plus the OS/architecture
# helpers in arch/ (the same set the Makefile and CMakeLists.txt build).
LOCAL_SRC_FILES := $(patsubst $(LOCAL_PATH)/%,%,\
	$(wildcard $(JENT_ROOT)/src/*.c $(JENT_ROOT)/arch/*.c))

include $(BUILD_SHARED_LIBRARY)
