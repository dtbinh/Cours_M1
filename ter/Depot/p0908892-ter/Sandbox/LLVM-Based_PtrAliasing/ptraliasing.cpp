//===--- ptraliasing.cpp ----------------------------------------*- C++ -*-===//
//                                    AUTHOR
// Name: Christophe Bacara
// Mail: christophe dot bacara at etudiant dot univ hyphen lille1 dot fr
// 
// Resp.: Laure Gonnord - INRIA/IRCICA - DaRT/Emeraude
//
//===----------------------------------------------------------------------===//
//
// TEST PURPOSE ONLY:
// Build aliasing matrices !
//
//===----------------------------------------------------------------------===//

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <vector>
#include <stack>
#include <unistd.h>

#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"   

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/APSInt.h"

#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/CompilerInstance.h"

#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/FileManager.h"  
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Diagnostic.h"

#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Lexer.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"

#include "clang/Parse/ParseAST.h"

#include "clang/Rewrite/Frontend/Rewriters.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include "clang/Analysis/CFG.h"
#include "clang/Analysis/Visitors/CFGRecStmtDeclVisitor.h"

using namespace clang;
using namespace llvm;
using namespace std;

//===----------------------------------------------------------------------===//
//                             CLASSES DEFINITIONS
//===----------------------------------------------------------------------===/
class AliasMatrixBuilder {
	private:
	// Pointer kind type definition
	typedef enum PointerKind_e { 
		// PK_NormalPointerPointer:
		// Represents a pointer declared as such in the source code.
		// Ex: void *ptr
		PK_NormalPointer,
		// PK_AnonymousPointerPointer:
		// Represents an anonymous pointer, which is the equivalent of AddrOf
		// expressions.
		// Ex: &var
		PK_AnonymousPointer
	} PointerKind;

	// Matrix kind type definition
	typedef enum MatrixKind_e {
		// MK_PointsToMatrix:
		// Represents a square matrix, with each line representing a "points-to"
		// relationship between two ponters, and each column representing a 
		// "pointed-by" relationship.
		MK_PointsToMatrix,
		// MK_SymetricalMatrix:
		// Represents a square matrix, which is a PointsToMatrix with symetric
		// closure.
		MK_SymetricalMatrix,
		// MK_ClosedMatrix:
		// Represents a square matrix, which is a SymetricalMatrix transitively
		// closed.
		MK_ClosedMatrix
	} MatrixKind;

	typedef bool** BooleanMatrix;

	map<MatrixKind, BooleanMatrix> m_matricesMap;

	map<VarDecl*, int> m_pointersMap;     // Mapping pointers with integer index
	map<VarDecl*, int> m_anonPointersMap; // Mapping var address (anon pointer) with integer index
	
	int m_dimension;                      // Size of map = dimension of square matrix

	bool m_matrixBuilt;                   // True if matrices have already been allocated
	bool m_matrixChanged;                 // True if any changed occured since last  transitive closure
	
	public:
	// Constructor
	AliasMatrixBuilder() : m_dimension(0){
		m_matricesMap[MK_PointsToMatrix] = NULL;
		m_matricesMap[MK_SymetricalMatrix] = NULL;
		m_matricesMap[MK_ClosedMatrix] = NULL;

		m_matrixBuilt = false;
		m_matrixChanged = false;
	}

	// Destructor
	~AliasMatrixBuilder() {
		typedef map<MatrixKind, BooleanMatrix>::iterator it;

		for (it b = m_matricesMap.begin(), e = m_matricesMap.end(); b != e; ++b) {
			for (int line = 0; line < m_dimension; ++line)
				free(b->second[line]);
			free(b->second);
		}
	}

	// Add a pointer into the internal map
	bool addPointer(VarDecl *pointer);
	// Add an anonymous pointer into the corresponding internal map
	bool addAnonymousPointer(VarDecl *var);
	// Set an alias between two pointers
	void setAlias(VarDecl *lhs, VarDecl *rhs);
	// Set an alias between a pointer and an anonymous pointer
	void setAliasFromAddrOf(VarDecl *lhs, VarDecl *rhsAddrOf);
	// Unsets all aliases bound with ptr
	void unsetAliases(VarDecl *ptr);
	// Return true if specified pointers are aliases
	bool getAlias(VarDecl *pointer1, VarDecl *pointer2);

	// Dump everything useful
	void dump();
	void dumpMatrix(BooleanMatrix matrix);

	private:
	// Allocates and build the matrix
	void build();
	// Set an alias
	void setAlias(VarDecl *lhs, VarDecl *rhs, PointerKind rhsKind);
	// Initialize the symetrical and closed matrices
	void initSymetricalMatrix();
	void initClosedMatrix();
	// Does a transitive closure
	void doClosure();
	// Check if matrix has changed, and if so, rebuild closed matrix
	void checkForChanges();
};

bool AliasMatrixBuilder::addPointer(VarDecl *pointer) {
	// If matrix already built, return false
	if (m_matrixBuilt)
		return false;

	// Add pointer into the map, set its index, and increment m_dimension
	m_pointersMap[pointer] = m_dimension++;
		return true;
}

bool AliasMatrixBuilder::addAnonymousPointer(VarDecl *var) {
	// If matrix already built, return false;
	if (m_matrixBuilt)
		return false;

	// Add anonymous pointer, set index on 0 for now
	m_anonPointersMap[var] = 0;
	return true;
}

void AliasMatrixBuilder::setAlias(VarDecl *lhs, VarDecl *rhs) {  
	setAlias(lhs, rhs, PK_NormalPointer);
}

void AliasMatrixBuilder::setAliasFromAddrOf(VarDecl *lhs, VarDecl *rhsAddrOf) {
	setAlias(lhs, rhsAddrOf, PK_AnonymousPointer);
}

void AliasMatrixBuilder::unsetAliases(VarDecl *pointer) {
	int line;

	// Getting pointer index
	line = m_pointersMap[pointer];

	// Setting false all along the corresponding line
	for (int col = 0; col < m_dimension; col++) {
		if (line != col)
			m_matricesMap[MK_PointsToMatrix][line][col] = false;
	}

	m_matrixChanged = true;
}

bool AliasMatrixBuilder::getAlias(VarDecl *pointer1, VarDecl *pointer2) {
	int x1, x2;

	checkForChanges();

	// Getting pointers indexes
	x1 = m_pointersMap[pointer1];
	x2 = m_pointersMap[pointer2];

	return m_matricesMap[MK_ClosedMatrix][x1][x2];
}

void AliasMatrixBuilder::dump() {  
	typedef map<VarDecl*, int>::iterator it_p;
	typedef map<MatrixKind, BooleanMatrix>::iterator it_m;

	checkForChanges();

	outs() << "----------------------------------------\n";
	outs()<< "AliasMatrixBuilder::dump() called.\n" ;
	outs()<< "Here are matrices indexes with matching pointers:\n";

	// Output all pointers within map
	for (it_p b = m_pointersMap.begin(), e = m_pointersMap.end(); b != e; ++b) {
		outs() << "\t" << b->second<< " => "<< b->first->getNameAsString()<< "\n";
	}
	// Output all anonymous pointers within map
	for (it_p b = m_anonPointersMap.begin(), e = m_anonPointersMap.end(); b != e; ++b) {
		outs() << "\t" << b->second<< " => &"<< b->first->getNameAsString()<< "\t[Anonymous pointer]\n";
	}
	outs() << "----------------------------------------\n";

	// Output aliases matrix
	for (it_m b = m_matricesMap.begin(), e = m_matricesMap.end(); b != e; ++b) {
		// Switch matrix' kind
		switch (b->first) {
		case MK_PointsToMatrix:
			outs() << "# Points-to & Pointed-by Matrix:\n";
			break;
		case MK_SymetricalMatrix:
			outs() << "# Symetrical Matrix:\n";
			break;
		case MK_ClosedMatrix:
			outs() << "# Aliases Matrix:\n";
			break;
		}

		dumpMatrix(b->second);

		outs() << "----------------------------------------\n";
	}
}

void AliasMatrixBuilder::dumpMatrix(BooleanMatrix matrix) {
	for (int line = 0; line < m_dimension; line++) {
		outs() << "\t";

		if (m_dimension > 1) {
			if (line == 0)
				outs() << "/ ";
			else if (line == m_dimension - 1)
				outs() << "\\ ";
			else
				outs() << "| ";
		}

		for (int col = 0; col < m_dimension; col++)
			outs() << (matrix[line][col] ? "1 " : "0 ");

		if (m_dimension > 1) {
			if (line == 0)
				outs() << "\\";
			else if (line == m_dimension - 1)
				outs() << "/";
			else
				outs() << "|";
		}

		outs() << "\n";
	}
}

void AliasMatrixBuilder::build() {
	typedef map<VarDecl*, int>::iterator it_p;
	typedef map<MatrixKind, BooleanMatrix>::iterator it_m;
	int sz;

	// Setting anonymous pointer indexes
	if (!m_anonPointersMap.empty())
		for (it_p b = m_anonPointersMap.begin(), e = m_anonPointersMap.end(); b != e; ++b)
			b->second = m_dimension++;

	sz = sizeof(bool) * m_dimension;

	for (it_m b = m_matricesMap.begin(), e = m_matricesMap.end(); b != e; ++b) {
		// Allocate bool* for each line
		b->second = (bool**)malloc(sizeof(bool*) * m_dimension);
		// Allocate each line
		for (int i = 0; i < m_dimension; ++i)
			b->second[i] = (bool*)malloc(sz);

		// Setting default values
		for (int line = 0; line < m_dimension; ++line)
			for (int col = 0; col < m_dimension; ++col)
				if (line == col)
					b->second[line][col] = true;
				else
					b->second[line][col] = false;
	}

	m_matrixBuilt = true;
	m_matrixChanged = true;
}

void AliasMatrixBuilder::setAlias(VarDecl *lhs, VarDecl *rhs, PointerKind rhsKind) { 
	int x1, x2;

	// If matrix already built, unset any existing alias bound with lhs
	if (m_matrixBuilt)
		unsetAliases(lhs);
	else // else, build matrix
		build();

	// Getting lhs and rhs indexes
	x1 = m_pointersMap[lhs];

	switch (rhsKind) {
	// Classical pointer
		case PK_NormalPointer:
			x2 = m_pointersMap[rhs];
			break;
		// Anonymous pointer
		case PK_AnonymousPointer:
			x2 = m_anonPointersMap[rhs];
			break;
	}

	// Setting matrix values
	m_matricesMap[MK_PointsToMatrix][x1][x2] = true;

	// Setting control boolean
	m_matrixChanged = true;
}

void AliasMatrixBuilder::initSymetricalMatrix() {
	// Setting each element of the symetric matrix
	for (int line = 0; line < m_dimension; line++)
		for (int col = 0; col < m_dimension; col++)
			if (m_matricesMap[MK_PointsToMatrix][line][col]|| m_matricesMap[MK_PointsToMatrix][col][line])
				m_matricesMap[MK_SymetricalMatrix][line][col] = true;
			else
				m_matricesMap[MK_SymetricalMatrix][line][col] = false;
}

void AliasMatrixBuilder::initClosedMatrix() {
	// Setting each element of the closed matrix on symetric one's values
	for (int line = 0; line < m_dimension; ++line)
		for (int col = 0; col < m_dimension; ++col)
			m_matricesMap[MK_ClosedMatrix][line][col] = 	m_matricesMap[MK_SymetricalMatrix][line][col];
}

void AliasMatrixBuilder::doClosure() {
	bool change = true;

	initSymetricalMatrix();
	initClosedMatrix();

	// Efficient closure algorithm
	// http://www.enseignement.polytechnique.fr/informatique/profs/
	//                                  Jean-Jacques.Levy/poly/main5/node5.html
	for (int x = 0, n = m_dimension; x < n; x++)
		for (int u = 0; u < n; u++)
			if (m_matricesMap[MK_ClosedMatrix][u][x])
				for (int v = 0; v < n; v++)
					if (m_matricesMap[MK_ClosedMatrix][x][v])
						m_matricesMap[MK_ClosedMatrix][u][v] = true;
}

void AliasMatrixBuilder::checkForChanges() {
	// If matrix not built, build it
	if (!m_matrixBuilt)
		build();

	// If matrix has changed, redo closure.
	if (m_matrixChanged)
		doClosure();
}

// CLASS: CFGPointerSeeker
// DESC:  
class CFGPointerSeeker : public CFGRecStmtVisitor<CFGPointerSeeker> {
	public:
	// Kind of visit
	typedef enum VisitKind { VK_PtrGatheringVisitKind, VK_AliasingVisitKind } VisitKind;

	private:

	SourceManager &m_SourceManager; // SourceManager used by the CompilerInstance
	ASTContext &m_ASTContext;       // AST Context when CFG builded.
	CFG &m_Cfg;                     // CFG to visit

	AliasMatrixBuilder m_matrixBuilder;
	VisitKind m_visitKind;

	public:
	// Constructor
	CFGPointerSeeker(SourceManager &smgr, FunctionDecl* f, CFG &cfg) : m_SourceManager(smgr), m_ASTContext(f->getASTContext()), m_Cfg(cfg) {
		// For params gathering
		typedef FunctionDecl::param_iterator it;
		for (it b = f->param_begin(), e = f->param_end(); b != e; ++b) {
			if (isa<PointerType>((*b)->getTypeSourceInfo()->getType().getTypePtr()))
				m_matrixBuilder.addPointer(*b);
		}

		m_visitKind = VK_PtrGatheringVisitKind;

	}

	// Template necessary stuff
	CFG &getCFG() { return m_Cfg; }
	void operator()(Stmt *S);

	// VisitKind Setter
	void setVisitKind(VisitKind kind) { m_visitKind = kind; }
	// Matrix dump
	void dumpAliases() { m_matrixBuilder.dump(); }

	// Top-Level statements visiting methods
	void BlockStmt_VisitStmt(Stmt *S);

	// Visiting methods
	void VisitBinaryOperator(BinaryOperator *BOP);
	void VisitDeclStmt(DeclStmt *DS);
};

void CFGPointerSeeker::operator()(Stmt *S) {
	BlockStmt_VisitStmt(S);
}

void CFGPointerSeeker::BlockStmt_VisitStmt(Stmt *S) {
	CFGRecStmtVisitor::Visit(S);
}

void CFGPointerSeeker::VisitBinaryOperator(BinaryOperator *BOP) {
	APSInt Result;
	UnaryOperator *UOP;
	IntegerLiteral *IL;
	DeclRefExpr *DRE;
	VarDecl *VD1, *VD2;
	Expr *E;

	switch (m_visitKind) {
		// First pass
		case VK_PtrGatheringVisitKind:
			if ((UOP = dyn_cast<UnaryOperator>(BOP->getRHS()->IgnoreParenCasts())) 
			&& (UOP->getOpcode() == UO_AddrOf)
			&& (DRE = dyn_cast<DeclRefExpr>(UOP->getSubExpr()->IgnoreParenCasts()))
			&& (VD1 = dyn_cast<VarDecl>(DRE->getDecl())))
			m_matrixBuilder.addAnonymousPointer(VD1);
			break;
		// Second pass
		case VK_AliasingVisitKind:
			// If LHS is a pointer ...
			if ((DRE = dyn_cast<DeclRefExpr>(BOP->getLHS()->IgnoreParenCasts()))
			&& (VD1 = dyn_cast<VarDecl>(DRE->getDecl()))
			&& (isa<PointerType>(VD1->getTypeSourceInfo()->getType().getTypePtr()))) {
				// If RHS is a pointer ...
				if ((DRE = dyn_cast<DeclRefExpr>(BOP->getRHS()->IgnoreParenCasts()))
				&& (VD2 = dyn_cast<VarDecl>(DRE->getDecl()))
				&& (isa<PointerType>(VD2->getTypeSourceInfo()->getType().getTypePtr()))) {
					m_matrixBuilder.setAlias(VD1, VD2);
				
				// Else, if RHS is an AddrOf expression
				} else if ((UOP = dyn_cast<UnaryOperator>(BOP->getRHS()->IgnoreParenCasts()))
				&& (UOP->getOpcode() == UO_AddrOf)
				&& (DRE = dyn_cast<DeclRefExpr>(UOP->getSubExpr()->IgnoreParenCasts()))
				&& (VD2 = dyn_cast<VarDecl>(DRE->getDecl()))) {
					m_matrixBuilder.setAliasFromAddrOf(VD1, VD2);      
				
				// Else, if RHS is a value 0 (aka, NULL)
				} else if ((IL = dyn_cast<IntegerLiteral>(BOP->getRHS()->IgnoreParenCasts()))
				&& IL->EvaluateAsInt(Result, m_ASTContext)
				&& Result == 0) {
				m_matrixBuilder.unsetAliases(VD1);
				}
			}
			break;    
	}
}

void CFGPointerSeeker::VisitDeclStmt(DeclStmt *DS) {
	// A l'interieur d'un CFG, les declarations sont toutes "inlinees".
	// Ainsi, pour chaque declaration existante, cette methode sera appellee.
	VarDecl *VD;
	VarDecl *VD_INIT;
	DeclRefExpr *DRE;
	Expr *E;
	const Type *TY;
	const PointerType *PTY;

	// S'il s'agit de la declaration d'un pointeur.
	if ((VD = dyn_cast<VarDecl>(DS->getSingleDecl()))) {
		TY = VD->getTypeSourceInfo()->getType().getTypePtr();
		if ((PTY = dyn_cast<PointerType>(TY))) {
			// Selon le type de visite
			switch (m_visitKind) {
				// First pass
				case VK_PtrGatheringVisitKind:
					m_matrixBuilder.addPointer(VD);
					break;
				// Second pass
				case VK_AliasingVisitKind:
					if ((E = VD->getInit())
					&& (DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenCasts()))
					&& (VD_INIT = dyn_cast<VarDecl>(DRE->getDecl()))
					&& (TY = VD_INIT->getTypeSourceInfo()->getType().getTypePtr())
					&& (PTY = dyn_cast<PointerType>(TY)))
						m_matrixBuilder.setAlias(VD, VD_INIT);
					break;
			}
		}
	}
}

//===----------------------------------------------------------------------===//
// CLASS: CustomRecursiveASTVisitor
// DESC:  Customized visitor template for AST. Visit any function declaration,
//        and build/visit the CFG if that function has a body.
//===----------------------------------------------------------------------===//
class CustomRecursiveASTVisitor : public RecursiveASTVisitor<CustomRecursiveASTVisitor> {
	private:
	Rewriter &m_Rewrite;  // C-Source code rewriter

	public:
	// Constructor
	CustomRecursiveASTVisitor(Rewriter &R) : m_Rewrite(R) { }

	// Visiting methods
	bool VisitFunctionDecl(FunctionDecl *f);
};

bool CustomRecursiveASTVisitor::VisitFunctionDecl(FunctionDecl *f) {
	if (f->hasBody()) {
		// Affichage !
		outs() << "########## Function with body found: "<< f->getNameInfo().getName().getAsString()<< " ##########\n";

		// Construction du CFG
		outs() << "Building Control-Flow Graph... ";
		CFG *cfg = CFG::buildCFG(f,	f->getBody(),	&f->getASTContext(),CFG::BuildOptions());
		outs() << "OK\n";

		cfg->dump(LangOptions(), true);

		// Instanciation du visitor
		outs() << "Creating CFG visitor... ";
		CFGPointerSeeker ps(m_Rewrite.getSourceMgr(), f, *cfg);
		outs() << "OK\n";

		outs() << "Visiting CFG...\n";

		// Premiere visite : reperage des declarations et &var
		cfg->VisitBlockStmts(ps);
		// Seconde visite : construction des matrices d'alias
		ps.setVisitKind(CFGPointerSeeker::VK_AliasingVisitKind);
		cfg->VisitBlockStmts(ps);
		// Dump des alias
		ps.dumpAliases();

		outs() << "CFG Visit ended.\n\n";

		// Liberation des ressources
		delete cfg;
	}

	return true;
}

//===----------------------------------------------------------------------===//
// CLASS: CustomASTConsumer
// DESC:  Customized template for AST consumering.
class CustomASTConsumer : public ASTConsumer {
	private:
	CustomRecursiveASTVisitor astvisitor; // AST Visitor to use

	public:
	// Constructor
	CustomASTConsumer(Rewriter &Rewrite) : astvisitor(Rewrite) { }

	// Explicit function name
	virtual bool HandleTopLevelDecl(DeclGroupRef d);
};

bool CustomASTConsumer::HandleTopLevelDecl(DeclGroupRef d) {
	typedef DeclGroupRef::iterator it;

	for (it b = d.begin(), e = d.end(); b != e; b++)
		astvisitor.TraverseDecl(*b);

	return true;
}
//===----------------------------------------------------------------------===//
//                             MAIN FUNCTION
//===----------------------------------------------------------------------===//

int main(int argc, char *argv[]) {
	struct stat filestat;

	// Verification de l'existence de l'argument FILE
	if (optind != (argc - 1)) {
		errs() << "Usage: cfgexplorer [OPTION] FILE\n";
		return 1;
	}

	// Verification de l'existence du fichier specifie
	std::string fname(argv[optind]);
	if (stat(fname.c_str(), &filestat) == -1) {
		perror(fname.c_str());
		exit(EXIT_FAILURE);
	}

	CompilerInstance compiler;
	DiagnosticOptions diagnosticOptions;
	TextDiagnosticPrinter *pTextDiagnosticPrinter = new TextDiagnosticPrinter(outs(),&diagnosticOptions,true);
	compiler.createDiagnostics(pTextDiagnosticPrinter);

	// bacara: <!> Pas entierement compris ce bout de code <!> 
	CompilerInvocation *Invocation = new CompilerInvocation;
	CompilerInvocation::CreateFromArgs(*Invocation, argv + 1, argv + argc,	compiler.getDiagnostics());
	compiler.setInvocation(Invocation);

	// Reglage de la cible
	llvm::IntrusiveRefCntPtr<TargetOptions> pto( new TargetOptions());
	pto->Triple = llvm::sys::getDefaultTargetTriple();
	llvm::IntrusiveRefCntPtr<TargetInfo>
	pti(TargetInfo::CreateTargetInfo(compiler.getDiagnostics(),
	pto.getPtr()));
	compiler.setTarget(pti.getPtr());

	compiler.createFileManager();
	compiler.createSourceManager(compiler.getFileManager());

	// <!>
	HeaderSearchOptions &headerSearchOptions = compiler.getHeaderSearchOpts();
	// <Warning!!> -- Platform Specific Code lives here
	// This depends on A) that you're running linux and
	// B) that you have the same GCC LIBs installed that
	// I do.
	// Search through Clang itself for something like this,
	// go on, you won't find it. The reason why is Clang
	// has its own versions of std* which are installed under
	// /usr/local/lib/clang/<version>/include/
	// See somewhere around Driver.cpp:77 to see Clang adding
	// its version of the headers to its include path.
	// To see what include paths need to be here, try
	// clang -v -c test.c
	// or clang++ for C++ paths as used below:

	//~ headerSearchOptions.AddPath("/usr/include/c++/4.6",
	//~ clang::frontend::Angled,
	//~ false,
	//~ false);
	//~ headerSearchOptions.AddPath("/usr/include/c++/4.6/i686-linux-gnu",
	//~ clang::frontend::Angled,
	//~ false,
	//~ false);
	//~ headerSearchOptions.AddPath("/usr/include/c++/4.6/backward",
	//~ clang::frontend::Angled,
	//~ false,
	//~ false);
	//~ headerSearchOptions.AddPath("/usr/local/include",
	//~ clang::frontend::Angled,
	//~ false,
	//~ false);
	//~ headerSearchOptions.AddPath("/usr/local/lib/clang/3.3/include",
	//~ clang::frontend::Angled,
	//~ false,
	//~ false);
	//~ headerSearchOptions.AddPath("/usr/include/i386-linux-gnu",
	//~ clang::frontend::Angled,
	//~ false,
	//~ false);
	//~ headerSearchOptions.AddPath("/usr/include",
	//~ clang::frontend::Angled,
	//~ false,
	//~ false);

	// </Warning!!> -- End of Platform Specific Code

	compiler.createPreprocessor();
	compiler.getPreprocessorOpts().UsePredefines = false;
	compiler.createASTContext();

	// Parametrage du Rewriter
	Rewriter Rewrite;
	Rewrite.setSourceMgr(compiler.getSourceManager(), compiler.getLangOpts());

	// Reglage du fichier d'entree
	const FileEntry *pFile = compiler.getFileManager().getFile(fname);
	compiler.getSourceManager().createMainFileID(pFile);
	compiler.getDiagnosticClient().BeginSourceFile(compiler.getLangOpts(),
	 &compiler.getPreprocessor());

	// Instanciation de l'ASTConsumer
	CustomASTConsumer astConsumer(Rewrite);

	// Parsing de l'AST + Visit
	ParseAST(compiler.getPreprocessor(),	&astConsumer,compiler.getASTContext());

	compiler.getDiagnosticClient().EndSourceFile();
	return 0;
}





