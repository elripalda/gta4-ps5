#pragma once

#include <rex/assert.h>

namespace rex::test {
template <class... Args> void Log(Args&&...) {}
}

#define REXFS_ERROR(...) ::rex::test::Log(__VA_ARGS__)
#define REXFS_WARN(...) ::rex::test::Log(__VA_ARGS__)
#define REXFS_DEBUG(...) ::rex::test::Log(__VA_ARGS__)
#define REXFS_TRACE(...) ::rex::test::Log(__VA_ARGS__)
#define REXLOG_ERROR(...) ::rex::test::Log(__VA_ARGS__)
#define REXLOG_WARN(...) ::rex::test::Log(__VA_ARGS__)
