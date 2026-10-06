#pragma once

struct SkyCraftTestLog
{
    template <typename T> SkyCraftTestLog& operator<<(const T&) { return *this; }
};
inline SkyCraftTestLog skycraftTestLog;
#define LOG(...) skycraftTestLog
