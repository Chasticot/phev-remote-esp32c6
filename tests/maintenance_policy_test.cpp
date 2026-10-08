#include "MaintenancePolicy.h"
#include <cassert>
#include <cstring>
#include <iostream>
int main() {
  assert(validBootMode(0)); assert(validBootMode(1)); assert(validBootMode(2)); assert(!validBootMode(3));
  uint8_t prefix[288] = {}; prefix[0]=0xe9; prefix[12]=13;
  prefix[32]=0x32;prefix[33]=0x54;prefix[34]=0xcd;prefix[35]=0xab;
  assert(validC6ApplicationPrefix(prefix,sizeof(prefix)));
  assert(!validC6ApplicationPrefix(prefix,35));
  prefix[12]=5; assert(!validC6ApplicationPrefix(prefix,sizeof(prefix)));
  prefix[12]=13;prefix[32]=0; assert(!validC6ApplicationPrefix(prefix,sizeof(prefix)));
  const auto bytes=reinterpret_cast<const uint8_t*>(PHEV_IMAGE_ID);
  for(size_t split=0;split<sizeof(PHEV_IMAGE_ID)-1;++split) {
    uint8_t matched=0; assert(!scanPhevIdentity(bytes,split,matched));
    assert(scanPhevIdentity(bytes+split,sizeof(PHEV_IMAGE_ID)-1-split,matched));
    assert(scanPhevIdentity(bytes,1,matched)); // deja identifie, sans hors-limites
  }
  uint8_t matched=0;
  assert(!scanPhevIdentity(reinterpret_cast<const uint8_t*>("not-a-phev-image"),16,matched));
  std::cout<<"PASS maintenance: modes, C6 application header, truncated/wrong chip/factory, fragmented identity marker\n";
}
