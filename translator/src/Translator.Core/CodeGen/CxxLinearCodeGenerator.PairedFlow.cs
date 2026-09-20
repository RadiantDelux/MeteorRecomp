using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    // false = scalar double, true = packed PS0/PS1, null = path-dependent.
    // A mixed CFG join must not destroy PS1 by converting the pair to a scalar,
    // nor round an incoming double by unconditionally converting it to a pair.
    // Only mixed values need a run-time representation bit; straight-line and
    // unambiguous flows retain the existing constant-foldable representation.
    private sealed record PairedFlowStateMaps(
        Dictionary<string, Dictionary<string, bool?>> In,
        Dictionary<string, Dictionary<string, bool?>> Out);

    private static PairedFlowStateMaps ComputePairedFlowStates(
        IrFunction func, IrCfg cfg, IGuestFunctionAbiProvider guestAbiProvider)
    {
        var blocks = func.Blocks;
        var indices = blocks.Select((block, index) => (block.Label, index))
            .ToDictionary(x => x.Label, x => x.index, StringComparer.OrdinalIgnoreCase);
        var predecessors = blocks.Select(block => cfg.Predecessors(block.Label)
            .Where(indices.ContainsKey).Select(label => indices[label]).ToArray()).ToArray();
        var scalarIn = new uint[blocks.Count];
        var pairedIn = new uint[blocks.Count];
        var scalarOut = new uint[blocks.Count];
        var pairedOut = new uint[blocks.Count];

        static uint Mask(string name)
        {
            var bit = PairedFloatRegisterBit(name);
            return bit < 0 ? 0u : 1u << bit;
        }

        // Both masks start at bottom. Joining all reachable predecessor facts
        // monotonically adds scalar/paired possibilities, including self loops.
        // The external entry contributes scalar state, even with a CFG backedge.
        bool changed;
        do
        {
            changed = false;
            for (var b = 0; b < blocks.Count; b++)
            {
                var block = blocks[b];
                var sin = block.Label.Equals(func.EntryLabel, StringComparison.OrdinalIgnoreCase)
                    || predecessors[b].Length == 0 ? uint.MaxValue : 0u;
                uint pin = 0;
                foreach (var p in predecessors[b])
                {
                    sin |= scalarOut[p];
                    pin |= pairedOut[p];
                }
                var s = sin;
                var pstate = pin;
                void Define(string destination, bool scalar, bool paired)
                {
                    var bit = Mask(destination);
                    s = (s & ~bit) | (scalar ? bit : 0);
                    pstate = (pstate & ~bit) | (paired ? bit : 0);
                }
                foreach (var instruction in block.Instructions)
                {
                    switch (instruction)
                    {
                        case IrPhi phi:
                        {
                            bool scalar = false, paired = false;
                            foreach (var (label, source) in phi.Sources)
                            {
                                var bit = Mask(source);
                                if (bit == 0 || !indices.TryGetValue(label, out var pred))
                                    scalar = true;
                                else
                                {
                                    scalar |= (scalarOut[pred] & bit) != 0;
                                    paired |= (pairedOut[pred] & bit) != 0;
                                }
                            }
                            Define(phi.Destination, scalar, paired);
                            break;
                        }
                        case IrAssign assign:
                        {
                            var source = assign.Value is { Kind: "register", RegisterName: { } name }
                                ? Mask(name) : 0;
                            Define(assign.Destination, source == 0 || (s & source) != 0,
                                source != 0 && (pstate & source) != 0);
                            break;
                        }
                        case IrCall call:
                        {
                            // ps_mr copies the register payload; it does not turn a
                            // scalar host double into a packed pair. Carry the same
                            // representation facts through joins and loop backedges.
                            if (call.Target.Equals("PPC_PsMr", StringComparison.OrdinalIgnoreCase) &&
                                call.Arguments.Count == 1 && !string.IsNullOrWhiteSpace(call.Destination))
                            {
                                var source = call.Arguments[0] is { Kind: "register", RegisterName: { } name }
                                    ? Mask(name) : 0;
                                Define(call.Destination, source == 0 || (s & source) != 0,
                                    source != 0 && (pstate & source) != 0);
                                break;
                            }
                            if (IsGuestCallTarget(call.Target))
                            {
                                var scalarArguments = ~ClearPairedGuestScalarFloatArguments(
                                    call.Target, call.Arguments, uint.MaxValue, guestAbiProvider);
                                s |= scalarArguments | (1u << 1);
                                pstate &= ~(scalarArguments | (1u << 1));
                            }
                            if (!string.IsNullOrWhiteSpace(call.Destination))
                            {
                                var paired = IsPairedProducerTarget(call.Target);
                                Define(call.Destination, !paired, paired);
                            }
                            break;
                        }
                        case IrIndirectCall call:
                            Define("f1", true, false);
                            if (!string.IsNullOrWhiteSpace(call.Destination))
                                Define(call.Destination, true, false);
                            break;
                        case IrResolvedPsqLoad load:
                            Define(load.Destination, false, true);
                            break;
                        default:
                            foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
                                Define(definition, true, false);
                            break;
                    }
                }
                if (scalarIn[b] != sin || pairedIn[b] != pin || scalarOut[b] != s || pairedOut[b] != pstate)
                {
                    scalarIn[b] = sin;
                    pairedIn[b] = pin;
                    scalarOut[b] = s;
                    pairedOut[b] = pstate;
                    changed = true;
                }
            }
        } while (changed);

        static Dictionary<string, bool?> Materialize(uint scalar, uint paired)
        {
            var result = new Dictionary<string, bool?>(StringComparer.OrdinalIgnoreCase);
            for (var bit = 0; bit < 32; bit++)
                if ((paired & (1u << bit)) != 0)
                    result[FprRegisterName(bit)] = (scalar & (1u << bit)) != 0 ? null : true;
            return result;
        }
        var inputs = new Dictionary<string, Dictionary<string, bool?>>(StringComparer.OrdinalIgnoreCase);
        var outputs = new Dictionary<string, Dictionary<string, bool?>>(StringComparer.OrdinalIgnoreCase);
        for (var b = 0; b < blocks.Count; b++)
        {
            inputs[blocks[b].Label] = Materialize(scalarIn[b], pairedIn[b]);
            outputs[blocks[b].Label] = Materialize(scalarOut[b], pairedOut[b]);
        }
        return new PairedFlowStateMaps(inputs, outputs);
    }

    private static string RuntimePairedTest(string register) =>
        $"((mkw_paired_state & 0x{1u << ParseFloatRegisterIndex(register):X8}u) != 0)";

    private static bool? PairedState(IReadOnlyDictionary<string, bool?> states, string register) =>
        states.TryGetValue(GetRegisterBaseName(register), out var paired) ? paired : false;
}
