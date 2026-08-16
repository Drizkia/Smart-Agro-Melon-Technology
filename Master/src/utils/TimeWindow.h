#ifndef TIME_WINDOW_H
#define TIME_WINDOW_H

#include <stdint.h>

// Apakah nowMinute (menit-hari, 0..1439) berada di dalam window [start, end)?
//   start == end  -> window kosong, selalu false
//   start <  end  -> window normal dalam satu hari
//   start >  end  -> window melewati tengah malam (mis. 23:30 -> 00:30)
//
// Ditulis sebagai satu ekspresi return agar tetap valid constexpr di C++11,
// sehingga bisa diuji lewat static_assert di Master/test/TimeWindowTests.h.
constexpr bool isMinuteInsideWindow(uint16_t nowMinute,
                                    uint16_t startMinute,
                                    uint16_t endMinute) {
    return (startMinute == endMinute)
        ? false
        : (startMinute < endMinute)
            ? (nowMinute >= startMinute && nowMinute < endMinute)
            : (nowMinute >= startMinute || nowMinute < endMinute);
}

#endif
