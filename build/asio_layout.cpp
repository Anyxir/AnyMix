#include <cstddef>
#include <cstdio>

struct Samples { unsigned long hi; unsigned long lo; };
struct TimeStamp { unsigned long hi; unsigned long lo; };
struct TimeCode {
  double speed;
  Samples timeCodeSamples;
  unsigned long flags;
  char future[64];
};
struct TimeInfo {
  double speed;
  TimeStamp systemTime;
  Samples samplePosition;
  double sampleRate;
  unsigned long flags;
  char reserved[12];
};
struct Time {
  long reserved[4];
  TimeInfo timeInfo;
  TimeCode timeCode;
};

int main() {
  std::printf("Time %zu info %zu code %zu\n", sizeof(Time), sizeof(TimeInfo), sizeof(TimeCode));
  std::printf("info %zu speed %zu sys %zu pos %zu rate %zu flags %zu\n", offsetof(Time, timeInfo),
              offsetof(TimeInfo, speed), offsetof(TimeInfo, systemTime), offsetof(TimeInfo, samplePosition),
              offsetof(TimeInfo, sampleRate), offsetof(TimeInfo, flags));
  std::printf("code %zu\n", offsetof(Time, timeCode));
  return 0;
}
