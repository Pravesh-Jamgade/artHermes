#ifndef OFFCHIP_PRED_FACTORY_H
#define OFFCHIP_PRED_FACTORY_H

#include <string>
#include "offchip_pred_base.h"

class OffchipPredFactory {
public:
    static OffchipPredBase* create_predictor(const std::string& type, uint32_t cpu, uint64_t seed);
};

#endif /* OFFCHIP_PRED_FACTORY_H */
