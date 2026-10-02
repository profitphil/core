using namespace QPI;

// Index reservation ONLY. On the live chain, CONTRACT_INDEX 30 is another
// already-deployed contract that is NOT part of this fork. This empty
// placeholder keeps the positional contract array consistent so QREWARDS
// lands at index 31. Do not add logic here.
struct QRWRSV302
{
};

struct QRWRSV30 : public ContractBase
{
    struct StateData
    {
    };

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
    }

    INITIALIZE()
    {
    }
};
