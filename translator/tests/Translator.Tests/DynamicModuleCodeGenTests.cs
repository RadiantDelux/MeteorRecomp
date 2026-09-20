using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

public class DynamicModuleCodeGenTests
{
    private static readonly DynamicModuleTemplateContext Template = new(
        "test-template",
        CanonicalImageBase: 0x71000000u,
        ImageSize: 0x1000u,
        CanonicalBssBase: 0x72000000u,
        BssSize: 0x100u);

    [Fact]
    public void DynamicTemplateUsesOffsetIdentityAndNeverEmitsFixedRegistration()
    {
        var code = Emit(
            0x71000100u,
            new IrReturn(null));

        Assert.Contains("dynmod_test_template_00000100", code, StringComparison.Ordinal);
        Assert.DoesNotContain("// RECOMP_REGISTRATION base", code, StringComparison.Ordinal);
        Assert.DoesNotContain("// RECOMP_REGISTRATION mod", code, StringComparison.Ordinal);
        Assert.DoesNotContain("extern \"C\" void func_71000100", code, StringComparison.Ordinal);
    }

    [Fact]
    public void InternalDirectCallAndPcDerivedLrUseRuntimeInstance()
    {
        var code = Emit(
            0x71000100u,
            new IrAssign("lr", IrValue.Imm(0x71000104u)),
            new IrCall(string.Empty, "func_71000200", []),
            new IrCall(string.Empty, "func_80500000", []),
            new IrReturn(null));

        Assert.Contains(
            "ctx->lr = DynamicModule::RuntimeAddressFromOffset(ctx, 0x00000104u);",
            code,
            StringComparison.Ordinal);
        Assert.Contains(
            "InvokeCurrentDynamicModuleFunction(0x00000200u, ctx);",
            code,
            StringComparison.Ordinal);
        Assert.DoesNotContain("InvokeDirectCpu<0x71000200u>(ctx);", code, StringComparison.Ordinal);
        Assert.Contains("InvokeDirectCpu<0x80500000u>(ctx);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void LinkRegisterMayPointExactlyOnePastTemplateImage()
    {
        var code = Emit(
            0x71000FFCu,
            new IrAssign("lr", IrValue.Imm(0x71001000u)),
            new IrReturn(null));

        Assert.Contains(
            "ctx->lr = DynamicModule::RuntimeAddressFromOffset(ctx, 0x00001000u);",
            code,
            StringComparison.Ordinal);
    }

    [Fact]
    public void ScalarAndPsqMemoryAddressesUseSegmentAwareRuntimeRebase()
    {
        var code = Emit(
            0x71000100u,
            new IrAssign("r4", IrValue.Imm(0x72000040u)),
            new IrLoad("r5", new IrAddress("r4", 0), 4),
            new IrStore(new IrAddress("r4", 4), IrValue.Register("r5"), 4),
            new IrCall("f1", "PPC_PsqL", [IrValue.Register("r4"), IrValue.Imm(0), IrValue.Imm(0)]),
            new IrCall(string.Empty, "PPC_PsqSt", [IrValue.Register("r4"), IrValue.Register("f1"), IrValue.Imm(0), IrValue.Imm(0)]),
            new IrResolveGuestMemoryRange("range", IrValue.Register("r4"), 0, 8, true, true),
            new IrResolvedPsqLoad("f1", "range", IrValue.Register("r4"), 0, 0, 0, null),
            new IrResolvedPsqStore("range", IrValue.Register("r4"), 0, IrValue.Register("f1"), 0, 0, null),
            new IrReturn(null));

        Assert.Contains("DynamicModule::RebaseCanonicalAddress(ctx, static_cast<uint32_t>(r4))", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqLGqrInline<0u, 0u>", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqStGqrInline<0u, 0u>", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqLResolvedInline<0u, 0u>", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqStResolvedInline<0u, 0u>", code, StringComparison.Ordinal);
    }

    [Fact]
    public void JumpTableComparesRuntimeSelectorInCanonicalTemplateSpace()
    {
        var function = new IrFunction(
            "dynamic_jump_table",
            "entry",
            [
                new IrBasicBlock("entry", [
                    new IrJumpTable("r3", [
                        new IrJumpTableCase(0x71000180u, "case_a"),
                        new IrJumpTableCase(0x71000190u, "case_b")])
                ]),
                new IrBasicBlock("case_a", [new IrReturn(null)]),
                new IrBasicBlock("case_b", [new IrReturn(null)])
            ]);

        var code = new CxxLinearCodeGenerator().Emit(
            0x71000100u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("dynamic_jump_table", ValueRepresentation.Void),
            Types(),
            dynamicModuleTemplate: Template);

        Assert.Contains("switch (DynamicModule::CanonicalAddressFromRuntime(ctx, static_cast<uint32_t>(r3)))", code, StringComparison.Ordinal);
        Assert.Contains("case 0x71000180u:", code, StringComparison.Ordinal);
        Assert.Contains("InvokeIndirectJump(r3, ctx);", code, StringComparison.Ordinal);
    }

    private static string Emit(uint entryPoint, params IrInstruction[] instructions)
    {
        var function = new IrFunction(
            "dynamic_test",
            "entry",
            [new IrBasicBlock("entry", instructions)]);
        return new CxxLinearCodeGenerator().Emit(
            entryPoint,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("dynamic_test", ValueRepresentation.Void),
            Types(),
            dynamicModuleTemplate: Template);
    }

    private static RepresentationEnvironment Types() => new(new Dictionary<string, ValueRepresentation>
    {
        ["lr"] = ValueRepresentation.UInt32,
        ["r3"] = ValueRepresentation.UInt32,
        ["r4"] = ValueRepresentation.UInt32,
        ["r5"] = ValueRepresentation.UInt32,
        ["f1"] = ValueRepresentation.Float64,
    });
}
