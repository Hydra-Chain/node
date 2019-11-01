#include "qtumDGP.h"
#include "validation.h"
#include "locktrip/dgp.h"

dev::eth::EVMSchedule QtumDGP::getGasSchedule(unsigned int blockHeight){
    dev::eth::EVMSchedule schedule = dev::eth::ConstantinopleSchedule;
    return schedule;
}

uint32_t QtumDGP::getBlockSize(unsigned int blockHeight){
    uint64_t blockSize = DEFAULT_BLOCK_SIZE_DGP;
    if(blockHeight < 2){ // For the first blocks calling dgp contract crashes
        return blockSize;
    }

    bool isParamVotedFor = false;
    Dgp dgp;
    dgp.isParamVoted(BLOCK_SIZE_DGP_PARAM, isParamVotedFor);
    if(isParamVotedFor){
        dgp.getDgpParam(BLOCK_SIZE_DGP_PARAM, blockSize);

        if(blockSize < MIN_BLOCK_SIZE_DGP || blockSize > MAX_BLOCK_SIZE_DGP){
            blockSize = DEFAULT_BLOCK_SIZE_DGP;
        }
    }

    return blockSize;
}

uint64_t QtumDGP::getMinGasPrice(unsigned int blockHeight){
    uint64_t result = DEFAULT_MIN_GAS_PRICE_DGP;
    return result;
}

uint64_t QtumDGP::getBlockGasLimit(unsigned int blockHeight){
    uint64_t blockGasLimit = DEFAULT_BLOCK_GAS_LIMIT_DGP;
    if(blockHeight < 2){ // For the first blocks calling dgp contract crashes
        return blockGasLimit;
    }

    bool isParamVotedFor = false;
    Dgp dgp;
    dgp.isParamVoted(BLOCK_SIZE_DGP_PARAM, isParamVotedFor);
    if(isParamVotedFor){
        dgp.getDgpParam(BLOCK_SIZE_DGP_PARAM, blockGasLimit);

        if(blockGasLimit < MIN_BLOCK_GAS_LIMIT_DGP || blockGasLimit > MAX_BLOCK_GAS_LIMIT_DGP){
            blockGasLimit = DEFAULT_BLOCK_GAS_LIMIT_DGP;
        }
    }

    return blockGasLimit;
}