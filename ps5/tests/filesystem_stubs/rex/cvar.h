#pragma once

// Registration is outside this test; filesystem classes and I/O use production code.
#define REXCVAR_DECLARE(type, name) extern type name
#define REXCVAR_DEFINE_BOOL(name, value, ...) bool name = value
