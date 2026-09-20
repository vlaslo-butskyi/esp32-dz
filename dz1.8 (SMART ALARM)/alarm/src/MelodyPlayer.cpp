
#include <MelodyPlayer.h>

MelodyPlayer::MelodyPlayer(const uint8_t pin) : pin_(pin) {}

void MelodyPlayer::init() {
    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, LOW);
}

void MelodyPlayer::start(
    const int (*notes)[2],
    size_t count,
    unsigned long tempoBpm
) {
  stop();

  if (notes == nullptr || count == 0 || tempoBpm == 0) {
    return;
  }

  // A zero denominator would cause division by zero.
  for (size_t i = 0; i < count; ++i) {
    if (notes[i][1] == 0) {
      return;
    }
  }

  notes_ = notes;
  count_ = count;
  index_ = 0;

  wholeNoteMs_ = 240000UL / tempoBpm;
  noteStartMs_ = millis();

  startNote();
}

void MelodyPlayer::startNote() {
  if (sounding_) noTone(pin_);
  sounding_ = false;

  const int frequency = notes_[index_][0];
  const long denominator = notes_[index_][1];

  const unsigned long divisor =
      denominator < 0
          ? static_cast<unsigned long>(-denominator)
          : static_cast<unsigned long>(denominator);

  noteDurationMs_ = wholeNoteMs_ / divisor;

  if (denominator < 0) {
    noteDurationMs_ = noteDurationMs_ * 3 / 2;
  }

  if (noteDurationMs_ == 0) {
    noteDurationMs_ = 1;
  }

  if (frequency > 0) {
    tone(pin_, frequency);
    sounding_ = true;
  }
}


void MelodyPlayer::stop() {
  if (notes_ != nullptr || sounding_) noTone(pin_);

  notes_ = nullptr;
  count_ = 0;
  index_ = 0;
  sounding_ = false;
}

void MelodyPlayer::update() {
  if (!isPlaying()) {
    return;
  }

  const unsigned long now = millis();

  // Catch up if a loop iteration took longer than one note.
  while (now - noteStartMs_ >= noteDurationMs_) {
    noteStartMs_ += noteDurationMs_;
    ++index_;

    if (index_ >= count_) {
      stop();
      return;
    }

    startNote();
  }

  // Sound for 90% of the note, then leave a short gap.
  const unsigned long soundDurationMs =
      noteDurationMs_ * 9 / 10;

  if (sounding_ && now - noteStartMs_ >= soundDurationMs) {
    noTone(pin_);
    sounding_ = false;
  }
}

bool MelodyPlayer::isPlaying() const {
  return notes_ != nullptr;
}

bool MelodyPlayer::isSounding() const {
  return isPlaying() && sounding_;
}