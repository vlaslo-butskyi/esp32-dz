#pragma once

// Format: {frequency in Hz, note denominator}
//
// 2  = half note
// 4  = quarter note
// 8  = eighth note
// 16 = sixteenth note
// 32 = thirty-second note
//
// Negative denominator = dotted note, duration multiplied by 1.5.
// Frequency 0 = rest.

namespace Melodies {
    // Nokia "Connecting People" power-on jingle, tempo 118, ~3 s
    // Last bar of Nokia Tune (B5 A5 C#5 E5 A5) moved down to F# major and played about twice as slow.
    // Notes and timing measured from a recording (https://soundboardguy.com/sounds/nokia-startup/),
    // octaves cross-checked with transcriptions https://onlinesequencer.net/3908349 and /4058511
    constexpr int NOKIA_STARTUP[][2] = {
        {1661, 8}, {1480, 8}, {932, 4}, {1109, 4},  // G#6 F#6 A#5 C#6
        {1480, -3},                                 // F#6, held ~1.5 s
    };

    // Verkhovna Rada voting board, tempo 56 (one beep every ~1.07 s)
    // Measured from https://zvukitop.com/zvuk-golosovaniia-verhovnoi-rady-skachat/ (vr-3, vr-4):
    // short beeps going down the Eb major scale while the vote is open, then a high G6 "ding" when it closes
    constexpr int RADA_BEEPS[][2] = {
        {622, 16}, {0, -8}, {587, 16}, {0, -8}, {523, 16}, {0, -8}, {466, 16}, {0, -8},  // Eb5 D5 C5 Bb4
        {415, 16}, {0, -8}, {392, 16}, {0, -8}, {349, 16}, {0, -8}, {311, 16}, {0, -8},  // Ab4 G4 F4 Eb4, ~8.6 s total
    };
    constexpr int RADA_DING[][2] = { {1568, -8}, {0, 16} };  // G6 rings ~0.7 s, then a short pause before the result

    // Our own result cues (the Rada board has no separate "accepted" / "not accepted" sounds), tempo 150
    constexpr int CUE_OK[][2] = { {1245, 16}, {1568, 16}, {1865, 16}, {2489, 4} };  // Eb6 G6 Bb6 Eb7: bright rising arpeggio, ~0.7 s
    constexpr int CUE_WRONG[][2] = { {784, 8}, {740, 8}, {698, 8}, {659, 2} };      // G5 F#5 F5 E5: falling "sad trombone", ~1.4 s

    // Dial-up modem, what the ALARM state plays. Tempo 375: 32 = 20 ms, 16 = 40 ms, -2 = 480 ms, loop ~1.5 s
    constexpr int MODEM_ALARM[][2] = {
        {1200, -2}, {2100, -2},                                      // дві несучі рукостискання
        {2600, 32}, {900, 16}, {3000, 32}, {1500, 16}, {2200, 32},   // далі "шум даних"
        {700, 16}, {2800, 32}, {1100, 16}, {1900, 32}, {2400, 16},
        {800, 32}, {3000, 16}, {1300, 32}, {2000, 16}, {650, 32},
        {2900, 16}, {1700, 32}, {2300, 16},
    };

} // namespace Melodies
