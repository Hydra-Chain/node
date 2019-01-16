#ifndef QTUMDGP_H
#define QTUMDGP_H

#include "qtumstate.h"
#include "primitives/block.h"
#include "validation.h"
#include "utilstrencodings.h"

class QtumDGP {
    
public:

    QtumDGP(QtumState* _state, bool _dgpevm = true) {}

    dev::eth::EVMSchedule getGasSchedule(unsigned int blockHeight);

    uint32_t getBlockSize(unsigned int blockHeight);

    uint64_t getMinGasPrice(unsigned int blockHeight);

    uint64_t getBlockGasLimit(unsigned int blockHeight);
};
#endif
