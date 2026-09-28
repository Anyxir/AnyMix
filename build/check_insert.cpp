#include <windows.h>
#include <cstdio>

int main() {
  const HMODULE dll = LoadLibraryW(L"C:\\Users\\akram\\Downloads\\vb\\build\\AnyMixInsertASIO.stage.dll");
  if (dll == nullptr) {
    std::printf("load %lu\n", GetLastError());
    return 1;
  }
  using Check = int (*)();
  const auto check = reinterpret_cast<Check>(GetProcAddress(dll, "AnyMix_CheckInsert"));
  if (check == nullptr) {
    std::printf("export missing\n");
    return 2;
  }
  const int code = check();
  std::printf("check %d\n", code);
  return code == 0 ? 0 : 3;
}
