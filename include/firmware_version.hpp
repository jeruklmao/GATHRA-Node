#pragma once

#define GATHRA_FIRMWARE_VERSION "2.1.1"
#define GATHRA_PROTOCOL_VERSION 3

#ifndef GATHRA_BUILD_ID
#if defined(GATHRA_ROLLBACK_TEST_FAIL)
#define GATHRA_BUILD_ID "rollback-test"
#elif defined(GATHRA_FORCE_MAINTENANCE)
#define GATHRA_BUILD_ID "hil"
#else
#define GATHRA_BUILD_ID "production"
#endif
#endif
