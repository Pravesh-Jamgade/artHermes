#include "offchip_pred_factory.h"
#include "offchip_oracle.h"
#include "offchip_pred_perc.h"
#include <iostream>

OffchipPredBase* OffchipPredFactory::create_predictor(const std::string& type, uint32_t cpu, uint64_t seed) {
    if (type == "oracle") {
        std::cout << "Creating PTW off-chip predictor: Oracle" << std::endl;
        return new OffchipOracle(cpu, type, seed);
    } else if (type == "perc") {
        std::cout << "Creating PTW off-chip predictor: Perceptron" << std::endl;
        return new OffchipPredPerc(cpu, type, seed);
    } else {
        std::cout << "Creating PTW off-chip predictor: Base (type: " << type << ")" << std::endl;
        return new OffchipPredBase(cpu, type, seed);
    }
}
