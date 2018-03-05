// g++ -std=c++17 -I /home/gburca/drw/heimdall/src/audit/ -g -o au au.cpp
// ./au | od -t c
// echo '{"key1":"value1","key2":-5000,"key3":false}' | ~/drw/au/au --encode - | od -t c
#include "AuEncoder.h"

#include <iostream>
#include <sstream>
#include <cmath>

using namespace std;

int main() {
  std::ostringstream os;
  Au au(os);

  au.encode([](AuFormatter &f) { f.map(); });
  au.encode([](AuFormatter &f) { f.map("key1", "value1", "key2", -5000, "keyToIntern3", false); });
  au.encode([](AuFormatter &f) { f.array(6, 1, 0, -7, -2, 5.9, -5.9); });
  au.encode([](AuFormatter &f) { f.array(); });

  auto NaNs = [](AuFormatter &f) { f.array(
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<long double>::quiet_NaN(), // converted to double
      0 / 0.0,
      std::sqrt(-1)
  ); };

  au.encode([&](AuFormatter &f) {
    f.map("NaNs", [&]() {
      NaNs(f);
    });
  });

  //au.encode([](AuFormatter &f) { f.value(-500); f.value(3987); }); // Invalid

  au.clearDictionary(false);
  au.encode([](AuFormatter &f) { f.map("key1", "value1", "key2", -5000, "keyToIntern3", false); });
  au.encode([](AuFormatter &f) {
    f.map("RepeatedVals",
      f.arrayVals([&]() {
        for (auto i = 0; i < 12; ++i) {
          f.value("valToIntern");
        }
      })
    );
  });
  au.encode([](AuFormatter &f) { f.value("valToIntern"); });

  cout << os.str();
}
