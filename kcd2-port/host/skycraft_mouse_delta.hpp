#pragma once

#include <atomic>

namespace skycraft
{
    // DirectInput state and buffered reads can expose the same physical motion.
    // Select one source for each capture/menu session and ignore the other.
    class MouseDelta
    {
    public:
        enum class Source { None, State, Buffered, RawData, RawBuffered };
        struct Sample { int x, y, wheel; };

        void Record(Source source, int x, int y, int wheel)
        {
            if (x || y) {
                auto expected = Source::None;
                source_.compare_exchange_strong(expected, source);
                if (source_.load() == source) {
                    x_.fetch_add(x);
                    y_.fetch_add(y);
                }
            }
            if (wheel) {
                auto expected = Source::None;
                wheelSource_.compare_exchange_strong(expected, source);
                if (wheelSource_.load() == source) wheel_.fetch_add(wheel);
            }
        }

        Sample Drain()
        {
            return {x_.exchange(0), y_.exchange(0), wheel_.exchange(0)};
        }

        void RetryWheel(int wheel) { wheel_.fetch_add(wheel); }
        Source Selected() const { return source_.load(); }

        void Reset()
        {
            x_.store(0);
            y_.store(0);
            wheel_.store(0);
            source_.store(Source::None);
            wheelSource_.store(Source::None);
        }

    private:
        std::atomic<Source> source_{Source::None};
        std::atomic<Source> wheelSource_{Source::None};
        std::atomic<int> x_{0}, y_{0}, wheel_{0};
    };
}
