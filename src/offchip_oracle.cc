#include <iostream>
#include "block.h"
#include "offchip_oracle.h"
#include "ooo_cpu.h"
#include "uncore.h"
#include "logging.h"

namespace knob
{
    extern float ocp_random_pos_rate;
}

void OffchipOracle::print_config()
{
   
}

void OffchipOracle::dump_stats()
{
    float precision = (true_pos + false_pos) > 0 ? (float)true_pos / (true_pos + false_pos) : 0.0f;
    float recall = (true_pos + false_neg) > 0 ? (float)true_pos / (true_pos + false_neg) : 0.0f;

    cout << "ocp_oracle_predict_called " << stats.pred.called << endl
         << "ocp_oracle_true_pos " << true_pos << endl
         << "ocp_oracle_false_pos " << false_pos << endl
         << "ocp_oracle_false_neg " << false_neg << endl
         << "ocp_oracle_true_neg " << true_neg << endl
         << "ocp_oracle_precision " << precision * 100.0f << endl
         << "ocp_oracle_recall " << recall * 100.0f << endl
         << endl;
}

void OffchipOracle::reset_stats()
{
    bzero(&stats, sizeof(stats));
    true_pos = 0;
    false_pos = 0;
    false_neg = 0;
    true_neg = 0;
}

OffchipOracle::OffchipOracle(uint32_t _cpu, string _type, uint64_t _seed) : OffchipPredBase(_cpu, _type, _seed)
{
    assert(knob::ocp_random_pos_rate <= 1.0);
    reset_stats();
}

OffchipOracle::~OffchipOracle()
{

}

// ================================================================================================

void OffchipOracle::set_state_info(PACKET* packet)
{
    // no state info to set for random predictor
}

void OffchipOracle::train(PACKET* packet)
{
    if(packet->went_offchip_pred && packet->went_offchip)           true_pos++;
    else if(packet->went_offchip_pred && !packet->went_offchip)     {
        false_pos++;
    }
    else if(!packet->went_offchip_pred && packet->went_offchip)     false_neg++;
    else if(!packet->went_offchip_pred && !packet->went_offchip)    true_neg++;
}

bool OffchipOracle::predict(PACKET* packet)
{
    stats.pred.called++;
    
    bool in_l1d = ooo_cpu[cpu].L1D.free_lookup(packet);
    bool in_l2 = ooo_cpu[cpu].L2C.free_lookup(packet);
    bool in_llc = uncore.LLC.free_lookup(packet);
    bool found_on_chip = (in_l1d || in_l2 || in_llc);
    bool prediction = !found_on_chip;
    
    packet->went_offchip_pred = prediction;
    if (prediction) {
        stats.pred.pos++;
    } else {
        stats.pred.neg++;
    }
    return prediction;
}

bool OffchipOracle::predict(ooo_model_instr *arch_instr, uint32_t data_index, LSQ_ENTRY *lq_entry)
{
    return false;
}

void OffchipOracle::train(ooo_model_instr *arch_instr, uint32_t data_index, LSQ_ENTRY *lq_entry)
{
}