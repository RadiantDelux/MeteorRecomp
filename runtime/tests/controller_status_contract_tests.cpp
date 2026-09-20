#include "hle/controller_status_contract.h"

int main()
{
    WpadContract::State state;
    if (state.GetLibraryStatus() != WpadContract::kStatusDisabled) return 1;
    if (state.GetDataFormat(0) != WpadContract::kErrorNotReady) return 2;
    if (state.SetDataFormat(0, 5) != WpadContract::kErrorNotReady) return 3;

    state.Initialize();
    if (state.GetLibraryStatus() != WpadContract::kStatusReady) return 4;
    if (state.GetDataFormat(0) != 0) return 5;
    if (state.SetDataFormat(0, 5) != 0) return 6;
    if (state.GetDataFormat(0) != 5) return 7;
    if (state.SetDataFormat(3, 8) != 0) return 8;
    if (state.GetDataFormat(3) != 8) return 9;
    if (state.GetDataFormat(4) != WpadContract::kErrorBadChannel) return 10;
    if (state.SetDataFormat(4, 2) != WpadContract::kErrorBadChannel) return 11;
    return 0;
}
