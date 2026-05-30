#ifndef OCP_ORACLE_H
#define OCP_ORACLE_H

#include <random>
#include "offchip_pred_base.h"

class OffchipOracle : public OffchipPredBase
{
    private:
        struct
        {
            struct
            {
                uint64_t called;
                uint64_t pos;
                uint64_t neg;
            } pred;
        } stats;

        uint64_t true_pos;
        uint64_t false_pos;
        uint64_t false_neg;
        uint64_t true_neg;

    public:
        OffchipOracle(uint32_t _cpu, string _type, uint64_t _seed);
        ~OffchipOracle();

        void print_config();
        void dump_stats();
        void reset_stats();
        void train(ooo_model_instr *arch_instr, uint32_t data_index, LSQ_ENTRY *lq_entry);
        bool predict(ooo_model_instr *arch_instr, uint32_t data_index, LSQ_ENTRY *lq_entry);
        bool predict(PACKET* packet);
        void train(PACKET* packet);
        void set_state_info(PACKET* packet);
};

#endif /* OFFCHIP_PRED_RANDOM_H */


