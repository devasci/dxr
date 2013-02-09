#include "dxr-docs.h"

#include "clang/AST/AST.h"
#include "clang/AST/Comment.h"
#include "clang/AST/CommentVisitor.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"

#include <stdlib.h>

using namespace llvm;
using namespace clang;

namespace dxr {

std::string escapeJSON(StringRef str) {
  std::string escaped(str);
  for (size_t i = 0; i < escaped.size(); ++i) {
    if (escaped[i] == '"' || escaped[i] == '\\') {
      escaped.insert(i, "\\");
      ++i;
    } else if (escaped[i] == '\n') {
      escaped.replace(i, 1, "\\n");
      ++i;
    } else if (escaped[i] == '\t') {
      escaped.replace(i, 1, "\\t");
      ++i;
    }
  }
  return escaped;
}

std::string convertToHTML(comments::FullComment *comment);

// This is a wrapper around NamedDecl::getQualifiedNameAsString.
// It produces more qualified output to distinguish several cases
// which would otherwise be ambiguous.
std::string getQualifiedName(const NamedDecl &d) {
  std::string ret;
  const DeclContext *ctx = d.getDeclContext();
  if (ctx->isFunctionOrMethod() && isa<NamedDecl>(ctx))
  {
    // This is a local variable.
    // d.getQualifiedNameAsString() will return the unqualifed name for this
    // but we want an actual qualified name so we can distinguish variables
    // with the same name but that are in different functions.
    ret = getQualifiedName(*cast<NamedDecl>(ctx)) + "::" + d.getNameAsString();
  }
  else
  {
    ret = d.getQualifiedNameAsString();
  }

  if (const FunctionDecl *fd = dyn_cast<FunctionDecl>(&d))
  {
    // This is a function.  getQualifiedNameAsString will return a string
    // like "ANamespace::AFunction".  To this we append the list of parameters
    // so that we can distinguish correctly between
    // void ANamespace::AFunction(int);
    //    and
    // void ANamespace::AFunction(float);
    ret += "(";
    const FunctionType *ft = fd->getType()->castAs<FunctionType>();
    if (const FunctionProtoType *fpt = dyn_cast<FunctionProtoType>(ft))
    {
      unsigned num_params = fd->getNumParams();
      for (unsigned i = 0; i < num_params; ++i) {
        if (i)
          ret += ", ";
        ret += fd->getParamDecl(i)->getType().getAsString();
      }

      if (fpt->isVariadic()) {
        if (num_params > 0)
          ret += ", ";
        ret += "...";
      }
    }
    ret += ")";
    if (ft->isConst())
      ret += " const";
  }

  return ret;
}

extern void printQualType(QualType type, StringRef name, std::string &answer,
    const PrintingPolicy &pp);

void appendTemplateArguments(const TemplateDecl *td, std::string &outstring) {
  const TemplateParameterList *tpl = td->getTemplateParameters();
  const PrintingPolicy &pp = td->getASTContext().getPrintingPolicy();
  outstring += "<";
  bool needsComma = false;
  for (auto it = tpl->begin(); it != tpl->end(); ++it) {
    if (needsComma)
      outstring += ", ";
    needsComma = true;

    const NamedDecl *param = *it;
    if (const TemplateTypeParmDecl *ttp = dyn_cast<TemplateTypeParmDecl>(param)) {
      outstring += (ttp->wasDeclaredWithTypename() ? "typename " : "class ");
      outstring += ttp->getName();
      if (ttp->hasDefaultArgument()) {
        outstring += " = ";
        printQualType(ttp->getDefaultArgument(), "", outstring, pp);
      }
    } else if (const NonTypeTemplateParmDecl *nttp =
        dyn_cast<NonTypeTemplateParmDecl>(param)) {
      printQualType(nttp->getType(), nttp->getName(), outstring, pp);
      // XXX: default arguments
    } else if (const TemplateTemplateParmDecl *ttp =
        dyn_cast<TemplateTemplateParmDecl>(param)) {
      outstring += "template ";
      appendTemplateArguments(ttp, outstring);
      outstring += " ";
      outstring += ttp->getName();
      // XXX: default arguments
    } else {
      errs() << "What is this if not a template param?\n";
      errs() << param->getDeclKindName() << '\n';
    }
  }
  outstring += ">";
}

class DocumentableEntity;
class ContainerEntity;

class DocumentableEntity {
public:
  enum DocumentedType {
    File, Namespace, Class, Leaf
  };

  DocumentableEntity(const Decl *source, DocumentedType t, StringRef container);

  const DocumentedType mDocType;

  void printJSON(raw_ostream &out);

  virtual ContainerEntity *containerize() { return NULL; }

  bool isContained() const { return mContained; }
  void setContained(bool contained) { mContained = contained; }
protected:
  virtual void printJSONFields(raw_ostream &out);

private:
  std::string mBriefDocumentation;
  std::string mFullDocumentation;
  bool mContained;
  std::string mContainerName;
};

class NamedEntity {
  std::string mQualifiedName;
  std::string mShortName;
  std::string mFile;
  uint32_t mLine, mColumn;

protected:
  virtual void printJSONFields(raw_ostream &out);

public:
  NamedEntity(NamedDecl *nd);
};

class ContainerEntity {
  std::vector<DocumentableEntity *> mEntities;

public:
  void addMemberEntity(DocumentableEntity *sub);
  virtual void printJSONFields(raw_ostream &out);

  static bool classof(const DocumentableEntity *ent) {
    return ent->mDocType != DocumentableEntity::Leaf;
  }
};

class NamespaceEntity : public DocumentableEntity,
                        public NamedEntity,
                        public ContainerEntity {
public:
  NamespaceEntity(NamespaceDecl *td);

  virtual ContainerEntity *containerize() { return this; }
  static bool classof(const DocumentableEntity *ent) {
    return ent->mDocType == Namespace;
  }

protected:
  virtual void printJSONFields(raw_ostream &out);
};

class ClassEntity : public DocumentableEntity,
                    public NamedEntity,
                    public ContainerEntity {
  const char *mKind;
  std::string mPreName;
  std::string mProlog;

public:
  ClassEntity(TagDecl *td, const char *kind, StringRef container);

  virtual ContainerEntity *containerize() { return this; }
  static bool classof(const DocumentableEntity *ent) {
    return ent->mDocType == Class;
  }

  void setPrename(StringRef prename) { mPreName = prename; }
  StringRef getPrename() const { return mPreName; }
  void setProlog(StringRef prolog) { mProlog = prolog; }
  StringRef getProlog() const { return mProlog; }

protected:
  virtual void printJSONFields(raw_ostream &out);
};

class LeafEntity : public DocumentableEntity,
                   public NamedEntity {
  std::string mPreName, mPostName;

public:
  LeafEntity(NamedDecl *d, StringRef container);

  StringRef getPreName() const { return mPreName; }
  void setPreName(StringRef preName) { mPreName = preName; }
  StringRef getPostName() const { return mPostName; }
  void setPostName(StringRef postName) { mPostName = postName; }

protected:
  virtual void printJSONFields(raw_ostream &out);
};

DocumentableEntity::DocumentableEntity(const Decl *source, DocumentedType t,
    StringRef container)
: mDocType(t), mContained(false), mContainerName(container) {
  ASTContext &ctxt = source->getASTContext();
  const RawComment *raw = ctxt.getRawCommentForAnyRedecl(source, NULL);
  if (raw) {
    mBriefDocumentation = raw->getBriefText(ctxt);
  }
  comments::FullComment *comment = ctxt.getCommentForDecl(source, NULL);
  if (comment)
    mFullDocumentation = convertToHTML(comment);
}

NamedEntity::NamedEntity(NamedDecl *nd)
: mQualifiedName(getQualifiedName(*nd)),
  mShortName(nd->getNameAsString())
{
  const SourceManager &sm = nd->getASTContext().getSourceManager();
  PresumedLoc loc = sm.getPresumedLoc(nd->getLocation());
  if (loc.isValid()) {
    mFile = loc.getFilename();
    mLine = loc.getLine();
    mColumn = loc.getColumn();
  }
}

NamespaceEntity::NamespaceEntity(NamespaceDecl *nd)
: DocumentableEntity(nd, DocumentableEntity::Namespace, "Namespaces"),
  NamedEntity(nd) {
}

ClassEntity::ClassEntity(TagDecl *td, const char *kind, StringRef container)
: DocumentableEntity(td, DocumentableEntity::Class, container),
  NamedEntity(td),
  mKind(kind) {
}

LeafEntity::LeafEntity(NamedDecl *d, StringRef container)
: DocumentableEntity(d, Leaf, container), NamedEntity(d) {
}

void ContainerEntity::addMemberEntity(DocumentableEntity *sub) {
  mEntities.push_back(sub);
  sub->setContained(true);
}

void ContainerEntity::printJSONFields(raw_ostream &out) {
  out << ",\"members\":[";
  bool needsComma = false;
  for (auto ent : mEntities) {
    if (needsComma)
      out << ",";
    needsComma = true;
    ent->printJSON(out);
  }
  out << "]";
}

void DocumentableEntity::printJSON(raw_ostream &out) {
  out << "{\"doctype\":\"";
  switch (mDocType) {
  case File: out << "file"; break;
  case Namespace: out << "namespace"; break;
  case Class: out << "aggregate"; break;
  case Leaf: out << "leaf"; break;
  }
  out << "\"";
  out << ",\"groupname\":\"" << mContainerName << "\"";
  printJSONFields(out);
  out << '}';
}

void DocumentableEntity::printJSONFields(raw_ostream &out) {
  out << ",\"briefdoc\":\"" << escapeJSON(mBriefDocumentation) << "\"";
  out << ",\"fulldoc\":\"" << escapeJSON(mFullDocumentation) << "\"";
}

void NamedEntity::printJSONFields(raw_ostream &out) {
  out << ",\"qualname\":\"" << mQualifiedName << '"';
  out << ",\"shortname\":\"" << mShortName << '"';
  out << ",\"location\":\"" << mFile << ":" <<
    mLine << ":" << mColumn << '"';
}

void NamespaceEntity::printJSONFields(raw_ostream &out) {
  DocumentableEntity::printJSONFields(out);
  NamedEntity::printJSONFields(out);
  ContainerEntity::printJSONFields(out);
}

void ClassEntity::printJSONFields(raw_ostream &out) {
  DocumentableEntity::printJSONFields(out);
  NamedEntity::printJSONFields(out);
  ContainerEntity::printJSONFields(out);
  out << ",\"prename\":\"" << mPreName << '"';
  out << ",\"kind\":\"" << mKind << '"';
  out << ",\"prolog\":\"" << mProlog << '"';
}

void LeafEntity::printJSONFields(raw_ostream &out) {
  DocumentableEntity::printJSONFields(out);
  NamedEntity::printJSONFields(out);
  out << ",\"prename\":\"" << mPreName << '"';
  out << ",\"postname\":\"" << mPostName << '"';
}

// This class is a general-purpose documentation generation class for code
// compiled by clang. We care about four main types of documentable entities:
// 1. Files (particularly header files)
//    The documentation we produce for a file consists of its header inclusion
//    hierarchy, per-file documentation comments, and the set of entities that
//    are declared within the file.
// 2. Namespaces
//    Namespaces can cross file boundaries...
// 3. Aggregate data structures (e.g., classes)
// 4. Leaf entities (macros, functions, variables)
//
// How do we generate documentation for an entity? These are structured like so:
// [file]:
//   type: file
//   path: path
//   briefdoc, fulldoc: ...
//   members: comma-separated list of docid
// [namespace]:
//   type: ???
//   qualname: mozilla::mailnews
//   tkind: namespace
//   shortname: mailnews
//   briefdoc, fulldoc: ...
//   members: comma-separated list of them
// [class]:
//   type: ???
//   qualname: mozilla::Outer::Inner
//   tkind: class
//   prename: private class
//   shortname: Outer::Inner
//   declprolog: class Outer::Inner : public Foo<Bar> (how to repr links?)
//   location: file:line:col
//   briefdoc, fulldoc: ...
//   members: comma-separated list of them
// [method,variable,etc]:
//   type: ???
//   qualname: clang::RecursiveASTVisitor
//   prename: template <typename T> void
//   shortname: addAnElement
//   postname: (T right, %Other% wrong) const
//   location: file:line:col
//   briefdoc, fulldoc: HTML descriptions
class DocGen : public RecursiveASTVisitor<DocGen> {
  DenseMap<const Decl *, DocumentableEntity *> documentedNodes;

  ContainerEntity *getContainerForDecl(const Decl *d);

  template <typename T> static T *getDef(T *decl) {
    T *def = decl->getDefinition();
    return def ? def : decl;
  }

public:

  bool VisitTagDecl(TagDecl *d);
  bool VisitEnumDecl(EnumDecl *d);
  bool VisitCXXRecordDecl(CXXRecordDecl *d);
  bool TraverseClassTemplateDecl(ClassTemplateDecl *ctd);
  bool VisitEnumConstantDecl(EnumConstantDecl *d);
  bool VisitFunctionDecl(FunctionDecl *d);
  bool VisitNamespaceDecl(NamespaceDecl *d);

  void extractDocumentation(raw_ostream &out);
};

ContainerEntity *DocGen::getContainerForDecl(const Decl *d) {
  const DeclContext *dc = d->getDeclContext();
  auto it = documentedNodes.find(dyn_cast<Decl>(dc)->getCanonicalDecl());
  if (it == documentedNodes.end())
    return NULL;
  return it->second->containerize();
}

bool DocGen::VisitTagDecl(TagDecl *d) {
  // Tag declarations correspond to enums, structs, et al. in C++.

  // Ignore non-canonical declarations
  if (!d->isCanonicalDecl())
    d = d->getCanonicalDecl();
  if (documentedNodes.find(d) != documentedNodes.end())
    return true;

  // If the declaration is anonymous, we do not want to document the type.
  if (d->getName().empty())
    return true;

  const char *containerName;
  switch (d->getTagKind()) {
  case TTK_Struct: case TTK_Class: containerName = "Classes"; break;
  case TTK_Union: containerName = "Unions"; break;
  case TTK_Enum: containerName = "Enumerations"; break;
  case TTK_Interface: containerName = "Interfaces"; break;
  }
  ClassEntity *ent = new ClassEntity(getDef(d), d->getKindName(), containerName);

  // XXX: prename = [access] [kind]
  std::string prename;
  prename += d->getKindName();
  ent->setPrename(prename);

  // Set the prolog for C-ish structs. This means we won't have to do anything
  // except for classes with inheritance or enums.
  std::string prolog(d->getKindName());
  prolog += ' ';
  prolog += d->getNameAsString();
  ent->setProlog(prolog);

  // Add this decl to its parent context
  ContainerEntity *cd = getContainerForDecl(d);
  if (cd)
    cd->addMemberEntity(ent);

  documentedNodes[d] = ent;
  return true;
}

bool DocGen::VisitEnumDecl(EnumDecl *d) {
  // Get the documented enum
  auto lookup = documentedNodes.find(d);
  if (lookup == documentedNodes.end())
    return true;
  ClassEntity *ent = dyn_cast<ClassEntity>(lookup->second);

  // The prolog of an enum is just enum [class] name [: underlying type]
  std::string prolog("enum ");
  if (d->isScopedUsingClassTag())
    prolog += "class ";
  prolog += d->getNameAsString();
  if (d->isFixed()) {
    prolog += " : ";
    printQualType(d->getIntegerType(), "", prolog,
      d->getASTContext().getPrintingPolicy());
  }
  ent->setProlog(prolog);

  return true;
}

bool DocGen::VisitCXXRecordDecl(CXXRecordDecl *d) {
  // Get the documented class
  auto lookup = documentedNodes.find(d);
  if (lookup == documentedNodes.end())
    return true;
  ClassEntity *ent = dyn_cast<ClassEntity>(lookup->second);

  // We don't need to do anything if there aren't any bases.
  if (!d->hasDefinition() || d->getNumBases() == 0)
    return true;

  const PrintingPolicy &pp = d->getASTContext().getPrintingPolicy();
  std::string prolog(ent->getProlog());
  prolog += " : ";
  bool needsComma = false;

  for (auto base = d->bases_begin(); base != d->bases_end(); ++base) {
    if (needsComma)
      prolog += ", ";
    needsComma = true;

    switch (base->getAccessSpecifier()) {
    case AS_public: prolog += "public "; break;
    case AS_protected: prolog += "protected "; break;
    case AS_private: prolog += "private "; break;
    case AS_none: break;
    }

    if (base->isVirtual())
      prolog += "virtual ";

    printQualType(base->getType(), "", prolog, pp);
  }
  ent->setProlog(prolog);

  return true;
}

// Handling template declarations: we want to modify the generated data for
// the templated pieces, so we need to do this after the inner traversal
// methods. This is why we do our job in Traverse* here and not Visit*.
bool DocGen::TraverseClassTemplateDecl(ClassTemplateDecl *ctd) {
  RecursiveASTVisitor<DocGen>::TraverseClassTemplateDecl(ctd);
  auto it = documentedNodes.find(ctd->getTemplatedDecl());
  if (it == documentedNodes.end()) {
    errs() << "Bad: " << ctd->getQualifiedNameAsString() << '\n';
    return true;
  }
  ClassEntity *ent = dyn_cast<ClassEntity>(it->second);
  std::string prolog;
  prolog += "template ";
  appendTemplateArguments(ctd, prolog);
  prolog += " ";
  prolog += ent->getProlog();
  ent->setProlog(prolog);
  return true;
}

bool DocGen::VisitEnumConstantDecl(EnumConstantDecl *d) {
  // This is a value within an enum.
  EnumDecl *parent = dyn_cast<EnumDecl>(d->getDeclContext());
  auto lookup = documentedNodes.find(parent);
  if (lookup == documentedNodes.end())
    // We are ignoring the enum this is in
    return true;

  LeafEntity *leaf = new LeafEntity(d, "Enum constants");
  std::string buildPostName;
  buildPostName = " = ";
  // XXX: When do I use hex, when do i use decimal?
  buildPostName += d->getInitVal().toString(10);
  leaf->setPostName(buildPostName);

  // Add the enum constant to the enum's list
  ClassEntity *ent = dyn_cast<ClassEntity>(lookup->second);
  ent->addMemberEntity(leaf);
  return true;
}

bool DocGen::VisitFunctionDecl(FunctionDecl *d) {
  if (!d->isCanonicalDecl())
    return true;

  // The basic format of a function in C++11 terms is the following:
  // [linkage] [template <>] [specifiers] type name(args...) [attrs]

  CXXMethodDecl *cxx = dyn_cast_or_null<CXXMethodDecl>(d);
  CXXConstructorDecl *construct = dyn_cast<CXXConstructorDecl>(d);
  CXXConversionDecl *convert = dyn_cast<CXXConversionDecl>(d);
  CXXDestructorDecl *destruct = dyn_cast<CXXDestructorDecl>(d);

  const PrintingPolicy &pp(d->getASTContext().getPrintingPolicy());

  const char *grouping = "Functions";
  if (construct)
    grouping = "Constructors";
  else if (destruct)
    grouping = "Destructors";
  else if (cxx)
    grouping = "Methods";
  LeafEntity *leaf = new LeafEntity(d, grouping);

  std::string preName, postName;

  // Add in specifiers for the function
  if (d->isExternC())
    preName += "extern \"C\" ";
  switch (d->getStorageClassAsWritten()) {
  case SC_None: break;
  case SC_Extern: preName += "extern "; break;
  case SC_Static: preName += "static "; break;
  default:
    llvm::errs() << "Unknown storage class!";
    return false;
  }
  if (d->isInlined())
    preName += "inline ";
  if (cxx && cxx->isVirtual())
    preName += "virtual ";
  if ((construct && construct->isExplicit()) ||
      (convert && convert->isExplicit()))
    preName += "explicit ";
  if (d->isConstexpr())
    preName += "constexpr ";

  // storage class, inline virtual explicit constexpr <type>
  printQualType(d->getResultType(), "", preName, pp);

  // Now build up the arguments list.
  bool needsComma = false;
  postName = "(";
  for (auto param = d->param_begin(); param != d->param_end(); ++param) {
    if (needsComma)
      postName += ", ";
    needsComma = true;

    printQualType((*param)->getType(), (*param)->getName(), postName, pp);
  }
  postName += ")";

  // cv-qualifiers
  if (cxx) {
    if (cxx->isConst())
      postName += " const";
    if (cxx->isVolatile())
      postName += " volatile";
  }

  // XXX: override, virtual, attributes

  // = 0, = default, = delete
  if (d->isDefaulted())
    postName += " = default";
  if (d->isDeleted())
    postName += " = delete";
  if (d->isPure())
    postName += " = 0";

  leaf->setPreName(preName);
  leaf->setPostName(postName);

  // Nowhere to add this to? Don't!
  ContainerEntity *cd = getContainerForDecl(d);
  if (!cd)
    return true;

  cd->addMemberEntity(leaf);

  return true;
}

bool DocGen::VisitNamespaceDecl(NamespaceDecl *d) {
  // Ignore non-canonical declarations
  if (!d->isCanonicalDecl())
    d = d->getCanonicalDecl();
  if (documentedNodes.find(d) != documentedNodes.end())
    return true;

  NamespaceEntity *ent = new NamespaceEntity(d);
  documentedNodes[d] = ent;
  
  // Add to the outer namespace, if present
  ContainerEntity *cd = getContainerForDecl(d);
  if (cd)
    cd->addMemberEntity(ent);
  return true;
}

void DocGen::extractDocumentation(raw_ostream &out) {
  bool needsComma = false;
  out << "[";
  for (auto node : documentedNodes) {
    // Only print nodes out once.
    if (node.second->isContained())
      continue;

    if (needsComma)
      out << ",";
    needsComma = true;
    node.second->printJSON(out);
  }
  out << "]";
}

void outputDocsForTU(TranslationUnitDecl *decl, raw_ostream &out) {
  DocGen documenter;
  documenter.TraverseDecl(decl);
  documenter.extractDocumentation(out);
}

namespace {
using namespace clang::comments;

class HTMLEmitter : public CommentVisitor<HTMLEmitter> {
  std::string mBuffer;
  const CommandTraits &traits;
public:
  HTMLEmitter(const CommandTraits &t) : traits(t) {}

  void visitComment(Comment *c) {
    errs() << "Unhandled comment kind " << c->getCommentKindName() << '\n';
    c->dump();
  }

  void visitBlockCommandComment(BlockCommandComment *c) {
    mBuffer += "@";
    mBuffer += c->getCommandName(traits);
    for (auto it = c->child_begin(); it != c->child_end(); ++it) {
      visit(*it);
    }
    mBuffer += "\n";
  }

  void visitFullComment(FullComment *c) {
    for (auto it : c->getBlocks()) {
      visit(it);
      mBuffer += "\n"; // Leave a blank line between blocks
    }
  }

  void visitHTMLStartTagComment(HTMLStartTagComment *c) {
    mBuffer += "<";
    mBuffer += c->getTagName();
    // XXX: HTML tag attributes
    if (c->isSelfClosing())
      mBuffer += "/";
    mBuffer += ">";
  }

  void visitHTMLEndTagComment(HTMLEndTagComment *c) {
    mBuffer += "</";
    mBuffer += c->getTagName();
    mBuffer += ">";
  }

  void visitParagraphComment(ParagraphComment *c) {
    for (auto it = c->child_begin(); it != c->child_end(); ++it) {
      visit(*it);
    }
    mBuffer += "\n"; // Close out a paragraph
  }

  void visitTextComment(TextComment *c) {
    mBuffer += c->getText();
    if (c->hasTrailingNewline())
      mBuffer += "\n";
  }

  const std::string &getOutput() const { return mBuffer; }
};
}

std::string convertToHTML(comments::FullComment *comment) {
  const ASTContext &ctx = comment->getDecl()->getASTContext();
  HTMLEmitter out(ctx.getCommentCommandTraits());
  out.visit(comment);
  return out.getOutput();
}
}
