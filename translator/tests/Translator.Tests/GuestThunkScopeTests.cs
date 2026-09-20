using Translator.Core.Analysis;
using Translator.Core.Loading;
using Xunit;

namespace Translator.Tests;

public sealed class GuestThunkScopeTests
{
    [Fact]
    public async Task ConcurrentProjectsKeepTheirOwnThunksInParallelWorkers()
    {
        var before = GuestSaveRestoreThunks.Current;
        var ready = new TaskCompletionSource[2];
        for (var i = 0; i < ready.Length; ++i)
            ready[i] = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var tasks = Enumerable.Range(0, 2).Select(index => Task.Run(async () =>
        {
            var address = 0x80001000u + (uint)index * 0x1000u;
            var map = FunctionMap.Parse([
                $"{address:X8} _save_gpr_14", $"{address + 68:X8} _save_gpr_31"
            ], "synthetic-project");
            using var scope = GuestSaveRestoreThunks.Use(GuestSaveRestoreThunks.FromFunctionMap(map));
            ready[index].SetResult();
            await Task.WhenAll(ready.Select(item => item.Task)).WaitAsync(TimeSpan.FromSeconds(10));
            Parallel.For(0, 64, _ => Assert.Equal(address, GuestSaveRestoreThunks.Current.SaveGpr!.BaseAddress));
            using (GuestSaveRestoreThunks.Use(GuestSaveRestoreThunks.None))
                Assert.True(GuestSaveRestoreThunks.Current.IsEmpty);
            Assert.Equal(address, GuestSaveRestoreThunks.Current.SaveGpr!.BaseAddress);
        }));
        await Task.WhenAll(tasks);
        Assert.Same(before, GuestSaveRestoreThunks.Current);
    }
}
