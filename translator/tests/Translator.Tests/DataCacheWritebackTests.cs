using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class DataCacheWritebackTests
{
    [Theory]
    [InlineData("dcbst", "PPC_Dcbst", 54)]
    [InlineData("dcbf", "PPC_Dcbf", 86)]
    [InlineData("xo_54", "PPC_Dcbst", 54)]
    [InlineData("xo_86", "PPC_Dcbf", 86)]
    public void WritebackHasAnObservableEffectiveAddress(string mnemonic, string helper, uint xo)
    {
        var raw = (31u << 26) | (4u << 16) | (5u << 11) | (xo << 1);
        var operands = mnemonic.StartsWith("xo_") ? Array.Empty<PpcOperand>() :
            new PpcOperand[] { new PpcRegisterOperand("r4", 4), new PpcRegisterOperand("r5", 5) };
        var instruction = PpcInstruction.Synthetic(0x80000000u, raw, mnemonic, operands);
        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        Assert.Equal(2, ir.Count);
        var address = Assert.IsType<IrBinary>(ir[0]);
        Assert.Equal("add", address.Op);
        Assert.Equal("r4", address.Left.RegisterName);
        Assert.Equal("r5", address.Right.RegisterName);
        var call = Assert.IsType<IrCall>(ir[1]);
        Assert.Equal(helper, call.Target);
        Assert.Equal(address.Destination, Assert.Single(call.Arguments).RegisterName);
    }

    [Theory]
    [InlineData("dcbst", 54)]
    [InlineData("dcbf", 86)]
    [InlineData("xo_54", 54)]
    [InlineData("xo_86", 86)]
    public void ZeroBaseDoesNotReadGprZero(string mnemonic, uint xo)
    {
        var raw = (31u << 26) | (5u << 11) | (xo << 1);
        var operands = mnemonic.StartsWith("xo_") ? Array.Empty<PpcOperand>() :
            new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r5", 5) };
        var instruction = PpcInstruction.Synthetic(0x80000000u, raw, mnemonic, operands);
        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var address = Assert.IsType<IrAssign>(ir[0]);
        Assert.Equal("r5", address.Value.RegisterName);
        Assert.IsType<IrCall>(ir[1]);
    }
}
