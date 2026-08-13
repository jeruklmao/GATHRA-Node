#pragma once

#define GATHRA_FIRMWARE_VERSION "1.0.0"
#define GATHRA_PROTOCOL_VERSION 1

#ifndef GATHRA_BUILD_ID
#if defined(GATHRA_ROLLBACK_TEST_FAIL)
#define GATHRA_BUILD_ID "rollback-test"
#elif defined(GATHRA_FORCE_MAINTENANCE)
#define GATHRA_BUILD_ID "hil"
#else
#define GATHRA_BUILD_ID "production"
#endif
#endif
