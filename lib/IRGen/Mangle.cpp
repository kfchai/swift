//===--- Mangle.cpp - Swift Name Mangling --------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements declaration name mangling in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"

#include "IRGen.h"
#include "IRGenModule.h"
#include "GenFunc.h"
#include "Linking.h"
#include "ValueWitness.h"

using namespace swift;
using namespace irgen;

/// Translate the given operator character into its mangled form.
///
/// Current operator characters:    /=-+*%<>!&|^~ and the special operator '..'
static char mangleOperatorChar(char op) {
  switch (op) {
  case '&': return 'a'; // 'and'
  case '/': return 'd'; // 'divide'
  case '=': return 'e'; // 'equal'
  case '>': return 'g'; // 'greater'
  case '<': return 'l'; // 'less'
  case '*': return 'm'; // 'multiply'
  case '!': return 'n'; // 'negate'
  case '|': return 'o'; // 'or'
  case '+': return 'p'; // 'plus'
  case '%': return 'r'; // 'remainder'
  case '-': return 's'; // 'subtract'
  case '^': return 'x'; // 'xor'
  case '~': return 't'; // 'tilde'
  case '.': return 'z'; // 'period'
  default: llvm_unreachable("bad identifier character");
  }
}

static bool isSwiftModule(Module *module) {
  return (!module->getParent() && module->Name.str() == "swift");
}

namespace {
  enum class IncludeType : bool { No, Yes };
  
  struct ArchetypeInfo {
    unsigned Depth;
    unsigned Index;
  };

  /// A helpful little wrapper for a value that should be mangled
  /// in a particular, compressed value.
  class Index {
    unsigned N;
  public:
    explicit Index(unsigned n) : N(n) {}
    friend raw_ostream &operator<<(raw_ostream &out, Index n) {
      if (n.N != 0) out << (n.N - 1);
      return (out << '_');
    }
  };

  /// A class for mangling declarations.
  class Mangler {
    raw_ostream &Buffer;
    llvm::DenseMap<void*, unsigned> Substitutions;
    llvm::DenseMap<ArchetypeType*, ArchetypeInfo> Archetypes;
    unsigned ArchetypesDepth = 0;

  public:
    Mangler(raw_ostream &buffer) : Buffer(buffer) {}
    void mangleContextOf(ValueDecl *decl);
    void mangleDeclContext(DeclContext *ctx);
    void mangleDeclName(ValueDecl *decl, IncludeType includeType);
    void mangleDeclType(ValueDecl *decl, ExplosionKind kind,
                        unsigned uncurryingLevel);
    void mangleEntity(ValueDecl *decl, ExplosionKind kind,
                      unsigned uncurryingLevel);
    void mangleNominalType(NominalTypeDecl *decl, ExplosionKind explosionKind);
    void mangleType(Type type, ExplosionKind kind, unsigned uncurryingLevel);
    void mangleDirectness(bool isIndirect);

  private:
    void mangleFunctionType(AnyFunctionType *fn, ExplosionKind explosionKind,
                            unsigned uncurryingLevel);
    void mangleProtocolList(ArrayRef<ProtocolDecl*> protocols);
    void mangleProtocolList(ArrayRef<Type> protocols);
    void mangleProtocolName(ProtocolDecl *protocol);
    void mangleIdentifier(Identifier ident);
    void mangleGetterOrSetterContext(FuncDecl *fn);
    void bindGenericParameters(const GenericParamList *genericParams,
                               bool mangleParameters);
    void manglePolymorphicType(const GenericParamList *genericParams, Type T,
                               ExplosionKind explosion, unsigned uncurryLevel,
                               bool mangleAsFunction);
    bool tryMangleStandardSubstitution(NominalTypeDecl *type);
    bool tryMangleSubstitution(void *ptr);
    void addSubstitution(void *ptr);
  };
}

/// Mangle an identifier into the buffer.
void Mangler::mangleIdentifier(Identifier ident) {
  StringRef str = ident.str();
  assert(!str.empty() && "mangling an empty identifier!");

  // Mangle normal identifiers as
  //   count identifier-char+
  // where the count is the number of characters in the identifier,
  // and where individual identifier characters represent themselves.
  if (!ident.isOperator()) {
    Buffer << str.size() << str;
    return;
  }

  // Mangle operator identifiers as
  //   'op' count operator-char+
  // where the count is the number of characters in the operator,
  // and where the individual operator characters are translated.
  Buffer << "op";

  Buffer << str.size();
  for (unsigned i = 0, e = str.size(); i != e; ++i) {
    Buffer << mangleOperatorChar(str[i]);
  }
}

bool Mangler::tryMangleSubstitution(void *ptr) {
  auto ir = Substitutions.find(ptr);
  if (ir == Substitutions.end()) return false;

  // substitution ::= 'S' integer? '_'

  unsigned index = ir->second;
  Buffer << 'S';
  if (index) Buffer << (index - 1);
  Buffer << '_';
  return true;
}

void Mangler::addSubstitution(void *ptr) {
  Substitutions.insert(std::make_pair(ptr, Substitutions.size()));
}

/// Mangle the context of the given declaration as a <context.
/// This is the top-level entrypoint for mangling <context>.
void Mangler::mangleContextOf(ValueDecl *decl) {
  auto clangDecl = decl->getClangDecl();

  // Classes published as Objective-C classes have a special context mangling.
  //   known-context ::= 'So'
  if (isa<ClassDecl>(decl) && (clangDecl || decl->isObjC())) {
    assert(!clangDecl || isa<clang::ObjCInterfaceDecl>(clangDecl));
    Buffer << "So";
    return;
  }

  // Otherwise, just mangle the decl's DC.
  mangleDeclContext(decl->getDeclContext());
}

void Mangler::mangleDeclContext(DeclContext *ctx) {
  switch (ctx->getContextKind()) {
  case DeclContextKind::BuiltinModule:
    llvm_unreachable("mangling member of builtin module!");

  case DeclContextKind::ClangModule:
    // Clang modules aren't namespaces, so there's nothing to mangle.
    // FIXME: This isn't right for C++, which does have namespaces,
    // but they aren't reflected into Swift anyway.
    return;

  case DeclContextKind::TranslationUnit: {
    Module *module = cast<Module>(ctx);

    // Try the special 'swift' substitution.
    // context ::= Ss
    if (isSwiftModule(module)) {
      Buffer << "Ss";
      return;
    }

    // context ::= substitution identifier*
    // context ::= identifier+

    if (tryMangleSubstitution(module)) return;

    if (DeclContext *parent = module->getParent())
      mangleDeclContext(parent);

    // This should work, because the language should be restricting
    // the name of a module to be a valid language identifier.
    mangleIdentifier(module->Name);
    addSubstitution(module);
    return;
  }

  case DeclContextKind::NominalTypeDecl:
    mangleNominalType(cast<NominalTypeDecl>(ctx), ExplosionKind::Minimal);
    return;

  case DeclContextKind::ExtensionDecl: {
    // Mangle the extension as the originally-extended type.
    Type type = cast<ExtensionDecl>(ctx)->getExtendedType();
    mangleType(type->getCanonicalType(), ExplosionKind::Minimal, 0);
    return;
  }

  case DeclContextKind::CapturingExpr:
    // FIXME: We need a real solution here for local types.
    if (FuncExpr *FE = dyn_cast<FuncExpr>(ctx)) {
      if (FE->getDecl()) {
        if (FE->getDecl()->isGetterOrSetter()) {
          mangleGetterOrSetterContext(FE->getDecl());
          return;
        }
        mangleDeclName(FE->getDecl(), IncludeType::Yes);
        return;
      }
    }
    llvm_unreachable("unnamed closure mangling not yet implemented");

  case DeclContextKind::ConstructorDecl:
    mangleDeclName(cast<ConstructorDecl>(ctx), IncludeType::Yes);
    return;

  case DeclContextKind::DestructorDecl:
    mangleDeclName(cast<DestructorDecl>(ctx), IncludeType::No);
    return;

  case DeclContextKind::TopLevelCodeDecl:
    // FIXME: I'm not sure this is correct.
    return;
  }

  llvm_unreachable("bad decl context");
}

void Mangler::mangleGetterOrSetterContext(FuncDecl *func) {
  assert(func->isGetterOrSetter());
  Decl *D = func->getGetterDecl();
  if (!D) D = func->getSetterDecl();
  assert(D && "no value type for getter/setter!");
  assert(isa<VarDecl>(D) || isa<SubscriptDecl>(D));

  mangleDeclName(cast<ValueDecl>(D), IncludeType::No);

  // We mangle the type with a canonical set of parameters because
  // objects nested within functions are shared across all expansions
  // of the function.
  mangleDeclType(cast<ValueDecl>(D), ExplosionKind::Minimal, /*uncurry*/ 0);

  if (func->getGetterDecl()) {
    Buffer << 'g';
  } else {
    Buffer << 's';
  }
}

/// Bind the generic parameters from the given list and its parents.
///
/// \param mangle if true, also emit the mangling for a 'generics'
void Mangler::bindGenericParameters(const GenericParamList *genericParams,
                                    bool mangle = false) {
  assert(genericParams);
  SmallVector<const GenericParamList *, 2> paramLists;
  
  // Determine the depth our parameter list is at. We don't actually need to
  // emit the outer parameters because they should have been emitted as part of
  // the outer context.
  const GenericParamList *parent = genericParams;
  do {
    ++ArchetypesDepth;
  } while ((parent = parent->getOuterParameters()));

  unsigned index = 0;

  for (auto archetype : genericParams->getAllArchetypes()) {
    // Remember the current depth and level.
    ArchetypeInfo info;
    info.Depth = ArchetypesDepth;
    info.Index = index++;
    assert(!Archetypes.count(archetype));
    Archetypes.insert(std::make_pair(archetype, info));

    if (!mangle) continue;

    // Mangle this type parameter.
    //   <generic-parameter> ::= <protocol-list> _
    // FIXME: Only mangle the archetypes and protocol requirements
    // that matter, rather than everything.
    mangleProtocolList(archetype->getConformsTo());
    Buffer << '_';
  }

  if (mangle) Buffer << '_';  
}

void Mangler::manglePolymorphicType(const GenericParamList *genericParams,
                                    Type T, ExplosionKind explosion,
                                    unsigned uncurryLevel,
                                    bool mangleAsFunction) {
  // FIXME: Prefix?
  llvm::SaveAndRestore<unsigned> oldArchetypesDepth(ArchetypesDepth);
  bindGenericParameters(genericParams, /*mangle*/ true);

  if (mangleAsFunction)
    mangleFunctionType(T->castTo<AnyFunctionType>(), explosion, uncurryLevel);
  else
    mangleType(T, explosion, uncurryLevel);
}

void Mangler::mangleDeclName(ValueDecl *decl, IncludeType includeType) {
  // decl ::= context identifier
  mangleContextOf(decl);
  mangleIdentifier(decl->getName());

  if (includeType == IncludeType::No) return;

  // We mangle the type with a canonical set of parameters because
  // objects nested within functions are shared across all expansions
  // of the function.
  mangleDeclType(decl, ExplosionKind::Minimal, /*uncurry*/ 0);
}

void Mangler::mangleDeclType(ValueDecl *decl, ExplosionKind explosion,
                             unsigned uncurryLevel) {
  // The return value here is a pair of (1) whether we need to mangle
  // the type and (2) whether we need to specifically bind parameters
  // from the context.
  typedef std::pair<bool, bool> result_t;
  struct ClassifyDecl : swift::DeclVisitor<ClassifyDecl, result_t> {
    /// TypeDecls don't need their types mangled in.
    result_t visitTypeDecl(TypeDecl *D) {
      return { false, false };
    }

    /// Function-like declarations do, but they should have
    /// polymorphic type and therefore don't need specific binding.
    result_t visitFuncDecl(FuncDecl *D) {
      return { true, false };
    }
    result_t visitConstructorDecl(ConstructorDecl *D) {
      return { true, false };
    }
    result_t visitDestructorDecl(DestructorDecl *D) {
      return { true, false };
    }

    /// All other values need to have contextual archetypes bound.
    result_t visitVarDecl(VarDecl *D) {
      return { true, true };
    }
    result_t visitSubscriptDecl(SubscriptDecl *D) {
      return { true, true };
    }
    result_t visitOneOfElementDecl(OneOfElementDecl *D) {
      return { true, D->hasArgumentType() };
    }

    /// Make sure we have a case for every ValueDecl.
    result_t visitValueDecl(ValueDecl *D) = delete;

    /// Everything else should be unreachable here.
    result_t visitDecl(Decl *D) {
      llvm_unreachable("not a ValueDecl");
    }
  };

  auto result = ClassifyDecl().visit(decl);
  assert(result.first || !result.second);

  // Bind the contextual archetypes if requested.
  llvm::SaveAndRestore<unsigned> oldArchetypesDepth(ArchetypesDepth);
  if (result.second) {
    auto genericParams = decl->getDeclContext()->getGenericParamsOfContext();
    if (genericParams) {
      bindGenericParameters(genericParams);
    }
  }

  // Mangle the type if requested.
  if (result.first) {
    mangleType(decl->getType(), explosion, uncurryLevel);
  }
}

/// Mangle a type into the buffer.
///
/// Type manglings should never start with [0-9_] or end with [0-9].
///
/// <type> ::= A <natural> <type>    # fixed-sized arrays
/// <type> ::= Bf <natural> _        # Builtin.Float
/// <type> ::= Bi <natural> _        # Builtin.Integer
/// <type> ::= BO                    # Builtin.ObjCPointer
/// <type> ::= Bo                    # Builtin.ObjectPointer
/// <type> ::= Bp                    # Builtin.RawPointer
/// <type> ::= Bu                    # Builtin.OpaquePointer
/// <type> ::= C <decl>              # class (substitutable)
/// <type> ::= F <type> <type>       # function type
/// <type> ::= f <type> <type>       # uncurried function type
/// <type> ::= G <type> <type>+ _    # bound generic type
/// <type> ::= O <decl>              # oneof (substitutable)
/// <type> ::= P <protocol-list> _   # protocol composition
/// <type> ::= Q <index>             # archetype with depth=0, index=N
/// <type> ::= Qd <index> <index>    # archetype with depth=M+1, index=N
/// <type> ::= R <type>              # lvalue
/// <type> ::= T <tuple-element>* _  # tuple
/// <type> ::= U <generic-parameter>+ _ <type>
/// <type> ::= V <decl>              # struct (substitutable)
///
/// <index> ::= _                    # 0
/// <index> ::= <natural> _          # N+1
///
/// <tuple-element> ::= <identifier>? <type>
void Mangler::mangleType(Type type, ExplosionKind explosion,
                         unsigned uncurryLevel) {
  TypeBase *base = type.getPointer();

  switch (base->getKind()) {
  case TypeKind::Error:
    llvm_unreachable("mangling error type");
  case TypeKind::UnstructuredUnresolved:
  case TypeKind::DeducibleGenericParam:
    llvm_unreachable("mangling unresolved type");
  case TypeKind::TypeVariable:
    llvm_unreachable("mangling type variable");

  case TypeKind::Module:
    llvm_unreachable("Cannot mangle module type yet");
      
  // We don't care about these types being a bit verbose because we
  // don't expect them to come up that often in API names.
  case TypeKind::BuiltinFloat:
    switch (cast<BuiltinFloatType>(base)->getFPKind()) {
    case BuiltinFloatType::IEEE16: Buffer << "Bf16_"; return;
    case BuiltinFloatType::IEEE32: Buffer << "Bf32_"; return;
    case BuiltinFloatType::IEEE64: Buffer << "Bf64_"; return;
    case BuiltinFloatType::IEEE80: Buffer << "Bf80_"; return;
    case BuiltinFloatType::IEEE128: Buffer << "Bf128_"; return;
    case BuiltinFloatType::PPC128: llvm_unreachable("ppc128 not supported");
    }
    llvm_unreachable("bad floating-point kind");
  case TypeKind::BuiltinInteger:
    Buffer << "Bi" << cast<BuiltinIntegerType>(base)->getBitWidth() << '_';
    return;
  case TypeKind::BuiltinRawPointer:
    Buffer << "Bp";
    return;
  case TypeKind::BuiltinOpaquePointer:
    Buffer << "Bu";
    return;
  case TypeKind::BuiltinObjectPointer:
    Buffer << "Bo";
    return;
  case TypeKind::BuiltinObjCPointer:
    Buffer << "BO";
    return;

#define SUGARED_TYPE(id, parent) \
  case TypeKind::id: \
    return mangleType(cast<id##Type>(base)->getDesugaredType(), \
                      explosion, uncurryLevel);
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"

  case TypeKind::MetaType:
    Buffer << 'M';
    return mangleType(cast<MetaTypeType>(base)->getInstanceType(),
                      ExplosionKind::Minimal, 0);

  case TypeKind::LValue:
    Buffer << 'R';
    return mangleType(cast<LValueType>(base)->getObjectType(),
                      ExplosionKind::Minimal, 0);

  case TypeKind::Tuple: {
    TupleType *tuple = cast<TupleType>(base);
    // type ::= 'T' tuple-field+ '_'
    // tuple-field ::= identifier? type
    Buffer << 'T';
    for (auto &field : tuple->getFields()) {
      if (field.hasName())
        mangleIdentifier(field.getName());
      mangleType(field.getType(), explosion, 0);
    }
    Buffer << '_';
    return;
  }

  case TypeKind::OneOf:
    return mangleNominalType(cast<OneOfType>(base)->getDecl(), explosion);

  case TypeKind::Protocol:
    return mangleNominalType(cast<ProtocolType>(base)->getDecl(), explosion);

  case TypeKind::Struct:
    return mangleNominalType(cast<StructType>(base)->getDecl(), explosion);

  case TypeKind::Class:
    return mangleNominalType(cast<ClassType>(base)->getDecl(), explosion);

  case TypeKind::UnboundGeneric:
    // We normally reject unbound types in IR-generation, but there
    // are several occasions in which we'd like to mangle them in the
    // abstract.
    mangleNominalType(cast<UnboundGenericType>(base)->getDecl(), explosion);
    return;

  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericOneOf:
  case TypeKind::BoundGenericStruct: {
    // type ::= 'G' <type> <type>+ '_'
    auto type = cast<BoundGenericType>(base);
    Buffer << 'G';
    mangleNominalType(type->getDecl(), explosion);
    for (auto arg : type->getGenericArgs()) {
      mangleType(arg, ExplosionKind::Minimal, /*uncurry*/ 0);
    }
    Buffer << '_';
    return;
  }

  case TypeKind::PolymorphicFunction: {
    // <type> ::= U <generic-parameter>+ _ <type>
    // 'U' is for "universal qualification".
    // The nested type is always a function type.
    PolymorphicFunctionType *fn = cast<PolymorphicFunctionType>(base);
    Buffer << 'U';
    manglePolymorphicType(&fn->getGenericParams(), fn, explosion, uncurryLevel,
                          /*mangleAsFunction=*/true);
    return;
  }

  case TypeKind::Archetype: {
    // <type> ::= Q <index>             # archetype with depth=0, index=N
    // <type> ::= Qd <index> <index>    # archetype with depth=M+1, index=N

    // Find the archetype information.  It may be possible for this to
    // fail for local declarations --- that might be okay; it means we
    // probably need to insert contexts for all the enclosing contexts.
    // And of course, linkage is not critical for such things.
    auto it = Archetypes.find(cast<ArchetypeType>(base));
    assert(it != Archetypes.end());
    auto &info = it->second;
    assert(ArchetypesDepth >= info.Depth);

    Buffer << 'Q';
    unsigned relativeDepth = ArchetypesDepth - info.Depth;
    if (relativeDepth != 0) {
      Buffer << 'd' << Index(relativeDepth - 1);
    }
    Buffer << Index(info.Index);
    return;
  }

  case TypeKind::Function:
    mangleFunctionType(cast<FunctionType>(base), explosion, uncurryLevel);
    return;

  case TypeKind::Array: {
    // type ::= 'A' integer type
    ArrayType *array = cast<ArrayType>(base);
    Buffer << 'A';
    Buffer << array->getSize();
    mangleType(array->getBaseType(), ExplosionKind::Minimal, 0);
    return;
  };

  case TypeKind::ProtocolComposition: {
    // We mangle ProtocolType and ProtocolCompositionType using the
    // same production:
    //   <type> ::= P <protocol-list> _
    // As a special case, if there is exactly one protocol in the
    // list, and it is a substitution candidate, then the *entire*
    // producton is substituted.

    auto protocols = cast<ProtocolCompositionType>(base)->getProtocols();
    assert(protocols.size() != 1);
    Buffer << 'P';
    mangleProtocolList(protocols);
    Buffer << '_';
    return;
  }
  }
  llvm_unreachable("bad type kind");
}

/// Mangle a list of protocols.  Each protocol is a substitution
/// candidate.
///   <protocol-list> ::= <protocol-name>+
void Mangler::mangleProtocolList(ArrayRef<Type> protocols) {
  for (auto protoTy : protocols) {
    mangleProtocolName(protoTy->castTo<ProtocolType>()->getDecl());
  }
}
void Mangler::mangleProtocolList(ArrayRef<ProtocolDecl*> protocols) {
  for (auto protocol : protocols) {
    mangleProtocolName(protocol);
  }
}

/// Mangle the name of a protocol as a substitution candidate.
void Mangler::mangleProtocolName(ProtocolDecl *protocol) {
  //  <protocol-name> ::= <decl>      # substitutable
  // The <decl> in a protocol-name is the same substitution
  // candidate as a protocol <type>, but it is mangled without
  // the surrounding 'P'...'_'.
  ProtocolType *type = cast<ProtocolType>(protocol->getDeclaredType());
  if (tryMangleSubstitution(type))
    return;
  mangleDeclName(protocol, IncludeType::No);
  addSubstitution(type);
}

static char getSpecifierForNominalType(NominalTypeDecl *decl) {
  switch (decl->getKind()) {
#define NOMINAL_TYPE_DECL(id, parent)
#define DECL(id, parent) \
  case DeclKind::id:
#include "swift/AST/DeclNodes.def"
    llvm_unreachable("not a nominal type");

  case DeclKind::Protocol: return 'P';
  case DeclKind::Class: return 'C';
  case DeclKind::OneOf: return 'O';
  case DeclKind::Struct: return 'V';
  }
  llvm_unreachable("bad decl kind");
}

void Mangler::mangleNominalType(NominalTypeDecl *decl,
                                ExplosionKind explosion) {
  // Check for certain standard types.
  if (tryMangleStandardSubstitution(decl))
    return;

  // For generic types, this uses the unbound type.
  TypeBase *key = decl->getDeclaredType().getPointer();

  // Try to mangle the entire name as a substitution.
  // type ::= substitution
  if (tryMangleSubstitution(key))
    return;

  Buffer << getSpecifierForNominalType(decl);
  mangleDeclName(decl, IncludeType::No);

  addSubstitution(key);
}

bool Mangler::tryMangleStandardSubstitution(NominalTypeDecl *decl) {
  // Bail out if our parent isn't the swift standard library.
  Module *parent = dyn_cast<Module>(decl->getDeclContext());
  if (!parent || !isSwiftModule(parent)) return false;

  // Standard substitutions shouldn't start with 's' (because that's
  // reserved for the swift module itself) or a digit or '_'.

  StringRef name = decl->getName().str();
  if (name == "Int64") {
    Buffer << "Si";
    return true;
  } else if (name == "UInt64") {
    Buffer << "Su";
    return true;
  } else if (name == "Bool") {
    Buffer << "Sb";
    return true;
  } else if (name == "Char") {
    Buffer << "Sc";
    return true;
  } else if (name == "Float64") {
    Buffer << "Sd";
    return true;
  } else if (name == "Float32") {
    Buffer << "Sf";
    return true;
  } else if (name == "String") {
    Buffer << "SS";
    return true;
  } else {
    return false;
  }
}

void Mangler::mangleFunctionType(AnyFunctionType *fn,
                                 ExplosionKind explosion,
                                 unsigned uncurryLevel) {
  // type ::= 'F' type type (curried)
  // type ::= 'f' type type (uncurried)
  // type ::= 'b' type type (objc block)
  if (isBlockFunctionType(fn))
    Buffer << 'b';
  else
    Buffer << (uncurryLevel > 0 ? 'f' : 'F');
  mangleType(fn->getInput(), explosion, 0);
  mangleType(fn->getResult(), explosion,
             (uncurryLevel > 0 ? uncurryLevel - 1 : 0));
}

void Mangler::mangleEntity(ValueDecl *decl, ExplosionKind explosion,
                           unsigned uncurryLevel) {
  mangleDeclName(decl, IncludeType::No);

  // Mangle in a type as well.  Note that we have to mangle the type
  // on all kinds of declarations, even variables, because at the
  // moment they can *all* be overloaded.
  mangleDeclType(decl, explosion, uncurryLevel);
}

static char mangleConstructorKind(ConstructorKind kind) {
  switch (kind) {
  case ConstructorKind::Allocating: return 'C';
  case ConstructorKind::Initializing: return 'c';
  }
  llvm_unreachable("bad constructor kind");
}

static StringRef mangleValueWitness(ValueWitness witness) {
  // The ones with at least one capital are the composite ops, and the
  // capitals correspond roughly to the positions of buffers (as
  // opposed to objects) in the arguments.  That doesn't serve any
  // direct purpose, but it's neat.
  switch (witness) {
  case ValueWitness::AllocateBuffer: return "al";
  case ValueWitness::AssignWithCopy: return "ac";
  case ValueWitness::AssignWithTake: return "at";
  case ValueWitness::DeallocateBuffer: return "de";
  case ValueWitness::Destroy: return "xx";
  case ValueWitness::DestroyBuffer: return "XX";
  case ValueWitness::InitializeBufferWithCopyOfBuffer: return "CP";
  case ValueWitness::InitializeBufferWithCopy: return "Cp";
  case ValueWitness::InitializeWithCopy: return "cp";
  case ValueWitness::InitializeBufferWithTake: return "Tk";
  case ValueWitness::InitializeWithTake: return "tk";
  case ValueWitness::ProjectBuffer: return "pr";

  case ValueWitness::Size:
  case ValueWitness::Alignment:
  case ValueWitness::Stride:
    llvm_unreachable("not a function witness");
  }
  llvm_unreachable("bad witness kind");
}

void Mangler::mangleDirectness(bool isIndirect) {
  Buffer << (isIndirect ? 'i': 'd');
}

/// Mangle this entity into the given buffer.
void LinkEntity::mangle(SmallVectorImpl<char> &buffer) const {
  llvm::raw_svector_ostream stream(buffer);
  mangle(stream);
}

/// Mangle this entity into the given stream.
void LinkEntity::mangle(raw_ostream &buffer) const {
  // Almost everything below gets the common prefix:
  //   mangled-name ::= '_T' global

  Mangler mangler(buffer);
  switch (getKind()) {
  // FIXME: Mangle a more descriptive symbol name for anonymous funcs.
  case Kind::AnonymousFunction:
    buffer << "closure";
    return;
      
  //   global ::= 'w' value-witness-kind type     // value witness
  case Kind::ValueWitness:
    buffer << "_Tw";
    buffer << mangleValueWitness(getValueWitness());
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;

  //   global ::= 'WV' type                       // value witness
  case Kind::ValueWitnessTable:
    buffer << "_TWV";
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;

  // Abstract type manglings just follow <type>.
  case Kind::TypeMangling:
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;

  //   global ::= 'M' directness type             // type metadata
  //   global ::= 'MP' directness type            // type metadata pattern
  case Kind::TypeMetadata: {
    buffer << "_TM";
    bool isPattern = isMetadataPattern();
    if (isPattern) buffer << 'P';
    mangler.mangleDirectness(isMetadataIndirect());
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;
  }

  //   global ::= 'Mm' type                       // class metaclass
  case Kind::SwiftMetaclassStub:
    buffer << "_TMm";
    mangler.mangleNominalType(cast<ClassDecl>(getDecl()),
                              ExplosionKind::Minimal);
    return;

  //   global ::= 'Wo' entity
  case Kind::WitnessTableOffset:
    buffer << "_TWo";
    mangler.mangleEntity(getDecl(), getExplosionKind(), getUncurryLevel());
    return;

  //   global ::= 'Wv' directness entity
  case Kind::FieldOffset:
    buffer << "_TWv";
    mangler.mangleDirectness(isOffsetIndirect());
    mangler.mangleEntity(getDecl(), ExplosionKind::Minimal, 0);
    return;
  
  //   global ::= 'Tb' type
  case Kind::BridgeToBlockConverter:
    buffer << "_TTb";
    mangler.mangleType(getType(), ExplosionKind::Minimal, 0);
    return;

  // For all the following, this rule was imposed above:
  //   global ::= local-marker? entity            // some identifiable thing

  //   entity ::= context 'D'                     // deallocating destructor
  //   entity ::= context 'd'                     // non-deallocating destructor
  case Kind::Destructor:
    buffer << "_T";
    if (isLocalLinkage()) buffer << 'L';
    mangler.mangleDeclContext(cast<ClassDecl>(getDecl()));
    switch (getDestructorKind()) {
    case DestructorKind::Deallocating:
      buffer << 'D';
      return;
    case DestructorKind::Destroying:
      buffer << 'd';
      return;
    }
    llvm_unreachable("bad destructor kind");

  //   entity ::= context 'C' type                // allocating constructor
  //   entity ::= context 'c' type                // non-allocating constructor
  case Kind::Constructor: {
    buffer << "_T";
    if (isLocalLinkage()) buffer << 'L';
    auto ctor = cast<ConstructorDecl>(getDecl());
    mangler.mangleContextOf(ctor);
    buffer << mangleConstructorKind(getConstructorKind());
    mangler.mangleDeclType(ctor, getExplosionKind(), getUncurryLevel());
    return;
  }

  //   entity ::= declaration                     // other declaration
  case Kind::Function:
    // As a special case, functions can have external asm names.
    if (!getDecl()->getAttrs().AsmName.empty()) {
      buffer << getDecl()->getAttrs().AsmName;
      return;
    }

    // Otherwise, fallthrough into the 'other decl' case.
    [[clang::fallthrough]];

  case Kind::Other:
    // As a special case, Clang functions and globals don't get mangled at all.
    // FIXME: When we can import C++, use Clang's mangler.
    if (auto clangDecl = getDecl()->getClangDecl()) {
      if (auto namedClangDecl = dyn_cast<clang::DeclaratorDecl>(clangDecl)) {
        buffer << namedClangDecl->getName();
        return;
      }
    }

    buffer << "_T";
    if (isLocalLinkage()) buffer << 'L';
    mangler.mangleEntity(getDecl(), getExplosionKind(), getUncurryLevel());
    return;

  //   entity ::= declaration 'g'                 // getter
  case Kind::Getter:
    buffer << "_T";
    if (isLocalLinkage()) buffer << 'L';
    mangler.mangleEntity(getDecl(), getExplosionKind(), getUncurryLevel());
    buffer << 'g';
    return;

  //   entity ::= declaration 's'                 // setter
  case Kind::Setter:
    buffer << "_T";
    if (isLocalLinkage()) buffer << 'L';
    mangler.mangleEntity(getDecl(), getExplosionKind(), getUncurryLevel());
    buffer << 's';
    return;

  // An Objective-C class reference;  not a swift mangling.
  case Kind::ObjCClass:
    buffer << "OBJC_CLASS_$_" << getDecl()->getName().str();
    return;

  // An Objective-C metaclass reference;  not a swift mangling.
  case Kind::ObjCMetaclass:
    buffer << "OBJC_METACLASS_$_" << getDecl()->getName().str();
    return;
  }
  llvm_unreachable("bad entity kind!");
}
