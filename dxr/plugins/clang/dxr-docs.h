#ifndef dxr_docs_h__
#define dxr_docs_h__

#include "llvm/ADT/DenseMap.h"

namespace clang {
  class TranslationUnitDecl;
}

namespace llvm {
  class raw_ostream;
}

namespace dxr {
/// Emits all of the documentation for the current compiler job.
void outputDocsForTU(clang::TranslationUnitDecl *decl, llvm::raw_ostream &out);
}
#endif
