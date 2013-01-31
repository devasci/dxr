#ifndef dxr_docs_h__
#define dxr_docs_h__

#include "llvm/ADT/DenseMap.h"
#include "clang/AST/RecursiveASTVisitor.h"

namespace clang {
  class CompilerInstance;
  class TranslationUnitDecl;
}

namespace llvm {
  class raw_ostream;
}

namespace dxr {

/// Emits all of the documentation for the current compiler job.
void outputDocsForTU(clang::TranslationUnitDecl *decl, llvm::raw_ostream &out);
using namespace llvm;
using namespace clang;

class ContainerEntity;
class DocumentableEntity;

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
}
#endif
