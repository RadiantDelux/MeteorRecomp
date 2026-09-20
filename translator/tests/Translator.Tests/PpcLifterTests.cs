using System;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class PpcLifterTests
{
    [Fact]
    public void LiftsMficcrIntoRegisterRead()
    {
        var lifter = new PpcLifter();
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "mficcr",
            new PpcOperand[] { new PpcRegisterOperand("r4", 4) });

        var lifted = lifter.Lift(new[] { instruction });

        var ir = Assert.Single(Assert.Single(lifted).Ir);
        var assign = Assert.IsType<IrAssign>(ir);
        Assert.Equal("r4", assign.Destination);
        Assert.Equal("iccr", assign.Value.RegisterName);
    }

    [Fact]
    public void LiftsCmpwWithBothOperands()
    {
        var lifter = new PpcLifter();
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0x7C032800,
            "cmpw",
            new PpcOperand[]
            {
                new PpcConditionRegisterOperand("cr0", 0),
                new PpcRegisterOperand("r3", 3),
                new PpcRegisterOperand("r5", 5)
            });

        var lifted = lifter.Lift(new[] { instruction });

        var sequence = Assert.Single(lifted).Ir;
        var setCr = Assert.IsType<IrSetCrField>(Assert.Single(sequence));
        Assert.Equal(0, setCr.FieldIndex);
        Assert.False(setCr.IsUnsigned);
        Assert.Equal("r3", setCr.Left.RegisterName);
        Assert.Equal("r5", setCr.Right.RegisterName);
    }

    [Fact]
    public void LiftsStwcxWithExplicitCrAndXerState()
    {
        var lifter = new PpcLifter();
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "stwcx.",
            new PpcOperand[]
            {
                new PpcRegisterOperand("r3", 3),
                new PpcRegisterOperand("r4", 4),
                new PpcRegisterOperand("r5", 5)
            });

        var lifted = lifter.Lift(new[] { instruction });

        var sequence = Assert.Single(lifted).Ir;
        var call = Assert.IsType<IrCall>(sequence.Last());
        Assert.Equal("PPC_Stwcx", call.Target);
        Assert.Equal("cr", call.Destination);
        Assert.Equal(4, call.Arguments.Count);
        Assert.Equal("r3", call.Arguments[1].RegisterName);
        Assert.Equal("cr", call.Arguments[2].RegisterName);
        Assert.Equal("xer", call.Arguments[3].RegisterName);
    }

    [Fact]
    public void UnsupportedInstructionProducesCommentWhenAllowed()
    {
        var lifter = new PpcLifter();
        var instruction = PpcInstruction.Synthetic(
            0x801D1FD4,
            0x7C00FD2C,
            "xo_662",
            Array.Empty<PpcOperand>());

        var lifted = lifter.Lift(new[] { instruction }, allowUnsupported: true);

        var irList = Assert.Single(lifted).Ir;
        Assert.NotEmpty(irList);
    }
}
