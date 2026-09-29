using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// Recovers concrete values that can reach arguments of direct calls in an SSA function.
/// This is evidence that translated guest PPC actually passes a value to another guest/helper
/// function; it is not a function-pointer oracle by itself. Phi nodes preserve alternatives so
/// branch-selected table bases are both retained.
/// </summary>
public static class CallArgumentConstantDiscovery
{
    private const int MaxAlternativesPerValue = 32;

    public static IReadOnlyList<uint> Discover(IrFunction ssaFunction)
    {
        ArgumentNullException.ThrowIfNull(ssaFunction);

        var definitions = new Dictionary<string, IrInstruction>(StringComparer.Ordinal);
        foreach (var instruction in ssaFunction.Blocks.SelectMany(static block => block.Instructions))
        {
            switch (instruction)
            {
                case IrAssign assign:
                    definitions[assign.Destination] = assign;
                    break;
                case IrBinary binary:
                    definitions[binary.Destination] = binary;
                    break;
                case IrPhi phi:
                    definitions[phi.Destination] = phi;
                    break;
            }
        }

        var memo = new Dictionary<string, HashSet<uint>?>(StringComparer.Ordinal);
        var visiting = new HashSet<string>(StringComparer.Ordinal);
        var discovered = new HashSet<uint>();

        foreach (var call in ssaFunction.Blocks
                     .SelectMany(static block => block.Instructions)
                     .OfType<IrCall>())
        {
            foreach (var argument in call.Arguments)
            {
                foreach (var value in ResolveValue(argument))
                {
                    discovered.Add(value);
                }
            }
        }

        return discovered.OrderBy(static value => value).ToArray();

        IReadOnlySet<uint> ResolveValue(IrValue value)
        {
            if (value.Kind == "const" && value.Constant is { } constant)
            {
                return new HashSet<uint> { unchecked((uint)constant) };
            }

            if (value.Kind != "register" || string.IsNullOrWhiteSpace(value.RegisterName))
            {
                return Empty();
            }

            return ResolveName(value.RegisterName);
        }

        IReadOnlySet<uint> ResolveName(string name)
        {
            if (memo.TryGetValue(name, out var cached))
            {
                return cached ?? Empty();
            }
            if (!visiting.Add(name) || !definitions.TryGetValue(name, out var definition))
            {
                return Empty();
            }

            HashSet<uint>? resolved = definition switch
            {
                IrAssign assign => Copy(ResolveValue(assign.Value)),
                IrBinary binary => ResolveBinary(binary),
                IrPhi phi => ResolvePhi(phi),
                _ => null,
            };

            visiting.Remove(name);
            memo[name] = resolved;
            return resolved ?? Empty();
        }

        HashSet<uint>? ResolvePhi(IrPhi phi)
        {
            var values = new HashSet<uint>();
            foreach (var source in phi.Sources.Values)
            {
                foreach (var value in ResolveName(source))
                {
                    values.Add(value);
                    if (values.Count > MaxAlternativesPerValue)
                    {
                        return null;
                    }
                }
            }
            return values;
        }

        HashSet<uint>? ResolveBinary(IrBinary binary)
        {
            if (binary.Op is not ("add" or "or" or "and" or "xor"))
            {
                return null;
            }

            var left = ResolveValue(binary.Left);
            var right = ResolveValue(binary.Right);
            if (left.Count == 0 || right.Count == 0)
            {
                return null;
            }

            var values = new HashSet<uint>();
            foreach (var lhs in left)
            {
                foreach (var rhs in right)
                {
                    var result = binary.Op switch
                    {
                        "add" => unchecked(lhs + rhs),
                        "or" => lhs | rhs,
                        "and" => lhs & rhs,
                        "xor" => lhs ^ rhs,
                        _ => throw new InvalidOperationException(),
                    };
                    values.Add(result);
                    if (values.Count > MaxAlternativesPerValue)
                    {
                        return null;
                    }
                }
            }
            return values;
        }

        static HashSet<uint> Copy(IReadOnlySet<uint> values) => new(values);
        static IReadOnlySet<uint> Empty() => Array.Empty<uint>().ToHashSet();
    }
}
