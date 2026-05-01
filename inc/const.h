#ifndef CONST_H
#define CONST_H

    enum hit_where_t
    {
        INV = 0,
        ITLB,
        ITLB_MSHR,
        DTLB,
        DTLB_MSHR,
        STLB, // STLB does not have MSHR. Anything that misses STLB directly emiulates PTW
        PTW,
        L1I,
        L1I_RQ,
        L1I_WQ,
        L1I_MSHR,
        L1D,
        L1D_RQ,
        L1D_WQ,
        L1D_MSHR,
        L2C,
        L2C_RQ,
        L2C_WQ,
        L2C_MSHR,
        LLC,
        LLC_RQ,
        LLC_WQ,
        LLC_MSHR,
        DRAM,

        NumHitWheres
    };

    extern const char* hit_where_names[];
#endif /* CONST_H */