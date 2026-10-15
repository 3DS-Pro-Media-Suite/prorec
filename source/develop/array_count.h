// The number of elements of an array (not of a pointer). Portable C.
#pragma once

#define ARRAY_COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))
