/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Interpreter.cpp
 *
 * Implementation of Souffle's interpreter.
 *
 ***********************************************************************/

#include "Interpreter.h"
#include "BTree.h"
#include "BinaryConstraintOps.h"
#include "FunctorOps.h"
#include "Global.h"
#include "IODirectives.h"
#include "IOSystem.h"
#include "InterpreterIndex.h"
#include "InterpreterRecords.h"
#include "Logger.h"
#include "ParallelUtils.h"
#include "ProfileEvent.h"
#include "RamExistenceCheckAnalysis.h"
#include "RamIndexScanKeys.h"
#include "RamNode.h"
#include "RamOperation.h"
#include "RamOperationDepth.h"
#include "RamProgram.h"
#include "RamProvenanceExistenceCheckAnalysis.h"
#include "RamValue.h"
#include "RamLatticeFunctor.h"
#include "RamVisitor.h"
#include "ReadStream.h"
#include "SignalHandler.h"
#include "SymbolTable.h"
#include "Util.h"
#include "WriteStream.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <typeinfo>
#include <utility>
#include <ffi.h>
#include "RamLatticeFunction.h"

namespace souffle {

/** Evaluate RAM Value */
RamDomain Interpreter::evalVal(const RamValue& value,
		const InterpreterContext& ctxt) {
	class ValueEvaluator: public RamVisitor<RamDomain> {
		Interpreter& interpreter;
		const InterpreterContext& ctxt;

	public:
		ValueEvaluator(Interpreter& interp, const InterpreterContext& ctxt) :
				interpreter(interp), ctxt(ctxt) {
		}

		RamDomain visitNumber(const RamNumber& num) override {
			return num.getConstant();
		}

		RamDomain visitElementAccess(const RamElementAccess& access) override {
			return ctxt[access.getIdentifier()][access.getElement()];
		}

		RamDomain visitLatticeGLB(const RamLatticeGLB& latGLB) override {
//			std::cout << "visit RamLatticeGLB here! ";
			InterpreterContext ctxt_temp;
			const RamLatticeBinaryFunction& glb_func =
					interpreter.getTranslationUnit().getProgram()->getLattice()->getGLB();

			const auto* refs = latGLB.getRefs();
			auto it = refs->begin();
			RamDomain res = ctxt[it->identifier][it->element];
//			std::cout << "it->identifier:" << it->identifier << ",it->element:" << it->element << "\n";
//			std::cout << "res: " << res << "\n";
			it++;
//			std::cout << "refs size:" << refs->size() << "\n";
			while (it != refs->end()) {
//				std::cout << "it->identifier:" << it->identifier << ",it->element:" << it->element << "\n";
				RamDomain it_r = ctxt[it->identifier][it->element];
//				std::cout << "last_res: " << res <<" ,it_r: " << it_r << "\n";
				// TODO
				// set 2 input variables
				std::vector<RamDomain> args = { res, it_r };
				ctxt_temp.setArguments(args);

				// evaluate binary constraint
				for (const auto& cas : glb_func.getLatCase()) {
					if (cas.match == nullptr
							|| interpreter.evalCond(*cas.match, ctxt_temp)) {
						// get output if match
						res = interpreter.evalVal(*cas.output, ctxt_temp);
						break;
					}
				}
				it++;
			}
//			std::cout << "visit RamLatticeGLB finish, res:" << res << "\n";
			return res;
		}

		RamDomain visitQuestionMark(const RamQuestionMark& qmark) override {
			if (interpreter.evalCond(qmark.getCondition(), ctxt)) {
//				std::cout<<"visitQuestionMark: cond pass\n";
				return interpreter.evalVal(qmark.getFirstRet(), ctxt);
			} else {
//				std::cout<<"visitQuestionMark: cond fail\n";
				return interpreter.evalVal(qmark.getSecondRet(), ctxt);
			}
			return 0;
		}

		RamDomain visitAutoIncrement(const RamAutoIncrement&) override {
			return interpreter.incCounter();
		}

		// intrinsic functors
		RamDomain visitIntrinsicOperator(const RamIntrinsicOperator& op)
				override {
			const auto& args = op.getArguments();

			switch (op.getOperator()) {
			/** Unary Functor Operators */
			case FunctorOp::ORD:
				return visit(args[0]);
			case FunctorOp::STRLEN:
				return interpreter.getSymbolTable().resolve(visit(args[0])).size();
			case FunctorOp::NEG:
				return -visit(args[0]);
			case FunctorOp::BNOT:
				return ~visit(args[0]);
			case FunctorOp::LNOT:
				return !visit(args[0]);
			case FunctorOp::TONUMBER: {
				RamDomain result = 0;
				try {
					result = stord(
							interpreter.getSymbolTable().resolve(
									visit(args[0])));
				} catch (...) {
					std::cerr << "error: wrong string provided by to_number(\"";
					std::cerr
							<< interpreter.getSymbolTable().resolve(
									visit(args[0]));
					std::cerr << "\") functor.\n";
					raise(SIGFPE);
				}
				return result;
			}
			case FunctorOp::TOSTRING:
				return interpreter.getSymbolTable().lookup(
						std::to_string(visit(args[0])));

				/** Binary Functor Operators */
			case FunctorOp::ADD: {
				return visit(args[0]) + visit(args[1]);
			}
			case FunctorOp::SUB: {
				return visit(args[0]) - visit(args[1]);
			}
			case FunctorOp::MUL: {
				return visit(args[0]) * visit(args[1]);
			}
			case FunctorOp::DIV: {
				return visit(args[0]) / visit(args[1]);
			}
			case FunctorOp::EXP: {
				return std::pow(visit(args[0]), visit(args[1]));
			}
			case FunctorOp::MOD: {
				return visit(args[0]) % visit(args[1]);
			}
			case FunctorOp::BAND: {
				return visit(args[0]) & visit(args[1]);
			}
			case FunctorOp::BOR: {
				return visit(args[0]) | visit(args[1]);
			}
			case FunctorOp::BXOR: {
				return visit(args[0]) ^ visit(args[1]);
			}
			case FunctorOp::LAND: {
				return visit(args[0]) && visit(args[1]);
			}
			case FunctorOp::LOR: {
				return visit(args[0]) || visit(args[1]);
			}
			case FunctorOp::MAX: {
				return std::max(visit(args[0]), visit(args[1]));
			}
			case FunctorOp::MIN: {
				return std::min(visit(args[0]), visit(args[1]));
			}
			case FunctorOp::CAT: {
				return interpreter.getSymbolTable().lookup(
						interpreter.getSymbolTable().resolve(visit(args[0]))
								+ interpreter.getSymbolTable().resolve(
										visit(args[1])));
			}

				/** Ternary Functor Operators */
			case FunctorOp::SUBSTR: {
				auto symbol = visit(args[0]);
				const std::string& str = interpreter.getSymbolTable().resolve(
						symbol);
				auto idx = visit(args[1]);
				auto len = visit(args[2]);
				std::string sub_str;
				try {
					sub_str = str.substr(idx, len);
				} catch (...) {
					std::cerr
							<< "warning: wrong index position provided by substr(\"";
					std::cerr << str << "\"," << (int32_t) idx << ","
							<< (int32_t) len << ") functor.\n";
				}
				return interpreter.getSymbolTable().lookup(sub_str);
			}

				/** Undefined */
			default: {
				assert(false && "unsupported operator");
				return 0;
			}
			}
		}

		RamDomain visitUserDefinedOperator(const RamUserDefinedOperator& op)
				override {
			// get name and type
			const std::string& name = op.getName();
			const std::string& type = op.getType();

			// load DLL (if not done yet)
			void* handle = interpreter.loadDLL();
			void (*fn)() = (void (*)())dlsym(handle, name.c_str());
			if (fn == nullptr) {
				std::cerr << "Cannot find user-defined operator " << name
						<< " in " << SOUFFLE_DLL << std::endl;
				exit(1);
			}

			// prepare dynamic call environment
			size_t arity = op.getArgCount();
			ffi_cif cif;
			ffi_type* args[arity];
			void* values[arity];
			RamDomain intVal[arity];
			const char* strVal[arity];
			ffi_arg rc;

			/* Initialize arguments for ffi-call */
			for (size_t i = 0; i < arity; i++) {
				RamDomain arg = visit(op.getArgument(i));
				if (type[i] == 'S') {
					args[i] = &ffi_type_pointer;
					strVal[i] =
							interpreter.getSymbolTable().resolve(arg).c_str();
					values[i] = &strVal[i];
				} else {
					args[i] = &ffi_type_uint32;
					intVal[i] = arg;
					values[i] = &intVal[i];
				}
			}

			// call external function
			if (type[arity] == 'N') {
				// Initialize for numerical return value
				if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, arity, &ffi_type_uint32,
						args) != FFI_OK) {
					std::cerr
							<< "Failed to prepare CIF for user-defined operator ";
					std::cerr << name << std::endl;
					exit(1);
				}
			} else {
				// Initialize for string return value
				if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, arity,
						&ffi_type_pointer, args) != FFI_OK) {
					std::cerr
							<< "Failed to prepare CIF for user-defined operator ";
					std::cerr << name << std::endl;
					exit(1);
				}
			}
			ffi_call(&cif, fn, &rc, values);
			RamDomain result;
			if (type[arity] == 'N') {
				result = ((RamDomain) rc);
			} else {
				result = interpreter.getSymbolTable().lookup(
						((const char*) rc));
			}

			return result;
		}

		RamDomain visitLatticeUnaryFunctor(const RamLatticeUnaryFunctor& luf)
				override {
//			std::cout << "visitLatticeUnaryFunctor here\n";

			InterpreterContext ctxt_temp;
			const RamLatticeUnaryFunction& func = luf.getFunc();
			RamDomain arg1 = interpreter.evalVal(*luf.getRef(), ctxt);

			// set 2 input variables
			std::vector<RamDomain> args = { arg1 };
			ctxt_temp.setArguments(args);

			// evaluate binary constraint
			for (const auto& cas : func.getLatCase()) {
				if (cas.constraint == nullptr
						|| interpreter.evalCond(*cas.constraint, ctxt_temp)) {
					// get output if match
					return interpreter.evalVal(*cas.output, ctxt_temp);
				}
			}

			std::cerr << "Failed to find a match for a lattice unary functor!"
					<< std::endl;
			exit(1);
		}

		RamDomain visitLatticeBinaryFunctor(const RamLatticeBinaryFunctor& lbf)
				override {
//			std::cout << "visitLatticeBinaryFunctor here\n";

			InterpreterContext ctxt_temp;
			const RamLatticeBinaryFunction& func = lbf.getFunc();
			RamDomain arg1 = interpreter.evalVal(*lbf.getRef1(), ctxt);
			RamDomain arg2 = interpreter.evalVal(*lbf.getRef2(), ctxt);

			// set 2 input variables
			std::vector<RamDomain> args = { arg1, arg2 };
			ctxt_temp.setArguments(args);

			// evaluate binary constraint
			for (const auto& cas : func.getLatCase()) {
				if (cas.match == nullptr
						|| interpreter.evalCond(*cas.match, ctxt_temp)) {
					// get output if match
//					std::cout << "find a match: " << *cas.output << std::endl;
//					if (dynamic_cast<RamQuestionMark *>(cas.output.get())!=nullptr) {
//						std::cout << "cast to question mark!\n";
//					}
					return interpreter.evalVal(*cas.output, ctxt_temp);
				}
			}

			std::cerr << "Failed to find a match for a lattice binary functor!"
					<< std::endl;
			exit(1);
		}

		// -- records --
		RamDomain visitPack(const RamPack& op) override {
			auto values = op.getArguments();
			auto arity = values.size();
			RamDomain data[arity];
			for (size_t i = 0; i < arity; ++i) {
				data[i] = visit(values[i]);
			}
			return pack(data, arity);
		}

		// -- subroutine argument
		RamDomain visitArgument(const RamArgument& arg) override {
			return ctxt.getArgument(arg.getArgCount());
		}

		// -- safety net --

		RamDomain visitNode(const RamNode& node) override {
			std::cerr << "Unsupported node type: " << typeid(node).name()
					<< "\n";
			assert(false && "Unsupported Node Type!");
			return 0;
		}
	};

	// create and run evaluator
	return ValueEvaluator(*this, ctxt)(value);
}

/** Evaluate RAM Condition */
bool Interpreter::evalCond(const RamCondition& cond,
		const InterpreterContext& ctxt) {
	class ConditionEvaluator: public RamVisitor<bool> {
		Interpreter& interpreter;
		const InterpreterContext& ctxt;
		RamExistenceCheckAnalysis* existCheckAnalysis;
		RamProvenanceExistenceCheckAnalysis* provExistCheckAnalysis;

	public:
		ConditionEvaluator(Interpreter& interp, const InterpreterContext& ctxt) :
				interpreter(interp), ctxt(ctxt), existCheckAnalysis(
						interp.getTranslationUnit().getAnalysis<
								RamExistenceCheckAnalysis>()), provExistCheckAnalysis(
						interp.getTranslationUnit().getAnalysis<
								RamProvenanceExistenceCheckAnalysis>()) {
		}

		// -- connectors operators --

		bool visitConjunction(const RamConjunction& conj) override {
			return visit(conj.getLHS()) && visit(conj.getRHS());
		}

		bool visitNegation(const RamNegation& neg) override {
			return !visit(neg.getOperand());
		}

		// -- relation operations --

		bool visitEmptinessCheck(const RamEmptinessCheck& emptiness) override {
			return interpreter.getRelation(emptiness.getRelation()).empty();
		}

		bool visitExistenceCheck(const RamExistenceCheck& exists) override {
			const InterpreterRelation& rel = interpreter.getRelation(
					exists.getRelation());

			// construct the pattern tuple
			auto arity = rel.getArity();
			auto values = exists.getValues();

			if (Global::config().has("profile")
					&& !exists.getRelation().isTemp()) {
				interpreter.reads[exists.getRelation().getName()]++;
			}
			// for total we use the exists test
			if (existCheckAnalysis->isTotal(&exists)) {
				RamDomain tuple[arity];
				for (size_t i = 0; i < arity; i++) {
					tuple[i] =
							(values[i]) ?
									interpreter.evalVal(*values[i], ctxt) :
									MIN_RAM_DOMAIN;
				}

				return rel.exists(tuple);
			}

			// for partial we search for lower and upper boundaries
			RamDomain low[arity];
			RamDomain high[arity];
			for (size_t i = 0; i < arity; i++) {
				low[i] = (values[i]) ? interpreter.evalVal(*values[i], ctxt) :
				MIN_RAM_DOMAIN;
				high[i] = (values[i]) ? low[i] : MAX_RAM_DOMAIN;
			}

			// obtain index
			auto idx = rel.getIndex(existCheckAnalysis->getKey(&exists));
			auto range = idx->lowerUpperBound(low, high);
			return range.first != range.second; // if there is something => done
		}

		bool visitProvenanceExistenceCheck(
				const RamProvenanceExistenceCheck& provExists) override {
			const InterpreterRelation& rel = interpreter.getRelation(
					provExists.getRelation());

			// construct the pattern tuple
			auto arity = rel.getArity();
			auto values = provExists.getValues();

			// for partial we search for lower and upper boundaries
			RamDomain low[arity];
			RamDomain high[arity];
			for (size_t i = 0; i < arity - 2; i++) {
				low[i] = (values[i]) ? interpreter.evalVal(*values[i], ctxt) :
				MIN_RAM_DOMAIN;
				high[i] = (values[i]) ? low[i] : MAX_RAM_DOMAIN;
			}

			low[arity - 2] = MIN_RAM_DOMAIN;
			low[arity - 1] = MIN_RAM_DOMAIN;
			high[arity - 2] = MAX_RAM_DOMAIN;
			high[arity - 1] = MAX_RAM_DOMAIN;

			// obtain index
			auto idx = rel.getIndex(
					provExistCheckAnalysis->getKey(&provExists));
			auto range = idx->lowerUpperBound(low, high);
			return range.first != range.second; // if there is something => done
		}

		// -- comparison operators --
		bool visitConstraint(const RamConstraint& relOp) override {
			RamDomain lhs = interpreter.evalVal(*relOp.getLHS(), ctxt);
			RamDomain rhs = interpreter.evalVal(*relOp.getRHS(), ctxt);
			switch (relOp.getOperator()) {
			case BinaryConstraintOp::EQ:
				return lhs == rhs;
			case BinaryConstraintOp::NE:
				return lhs != rhs;
			case BinaryConstraintOp::LT:
				return lhs < rhs;
			case BinaryConstraintOp::LE:
				return lhs <= rhs;
			case BinaryConstraintOp::GT:
				return lhs > rhs;
			case BinaryConstraintOp::GE:
				return lhs >= rhs;
			case BinaryConstraintOp::MATCH: {
				RamDomain l = interpreter.evalVal(*relOp.getLHS(), ctxt);
				RamDomain r = interpreter.evalVal(*relOp.getRHS(), ctxt);
				const std::string& pattern =
						interpreter.getSymbolTable().resolve(l);
				const std::string& text = interpreter.getSymbolTable().resolve(
						r);
				bool result = false;
				try {
					result = std::regex_match(text, std::regex(pattern));
				} catch (...) {
					std::cerr << "warning: wrong pattern provided for match(\""
							<< pattern << "\",\"" << text << "\").\n";
				}
				return result;
			}
			case BinaryConstraintOp::NOT_MATCH: {
				RamDomain l = interpreter.evalVal(*relOp.getLHS(), ctxt);
				RamDomain r = interpreter.evalVal(*relOp.getRHS(), ctxt);
				const std::string& pattern =
						interpreter.getSymbolTable().resolve(l);
				const std::string& text = interpreter.getSymbolTable().resolve(
						r);
				bool result = false;
				try {
					result = !std::regex_match(text, std::regex(pattern));
				} catch (...) {
					std::cerr << "warning: wrong pattern provided for !match(\""
							<< pattern << "\",\"" << text << "\").\n";
				}
				return result;
			}
			case BinaryConstraintOp::CONTAINS: {
				RamDomain l = interpreter.evalVal(*relOp.getLHS(), ctxt);
				RamDomain r = interpreter.evalVal(*relOp.getRHS(), ctxt);
				const std::string& pattern =
						interpreter.getSymbolTable().resolve(l);
				const std::string& text = interpreter.getSymbolTable().resolve(
						r);
				return text.find(pattern) != std::string::npos;
			}
			case BinaryConstraintOp::NOT_CONTAINS: {
				RamDomain l = interpreter.evalVal(*relOp.getLHS(), ctxt);
				RamDomain r = interpreter.evalVal(*relOp.getRHS(), ctxt);
				const std::string& pattern =
						interpreter.getSymbolTable().resolve(l);
				const std::string& text = interpreter.getSymbolTable().resolve(
						r);
				return text.find(pattern) == std::string::npos;
			}
			default:
				assert(false && "unsupported operator");
				return false;
			}
		}

		bool visitNode(const RamNode& node) override {
			std::cerr << "Unsupported node type: " << typeid(node).name()
					<< "\n";
			assert(false && "Unsupported Node Type!");
			return false;
		}
	};

	// run evaluator
	return ConditionEvaluator(*this, ctxt)(cond);
}

/** Evaluate RAM operation */
void Interpreter::evalOp(const RamOperation& op,
		const InterpreterContext& args) {
	class OperationEvaluator: public RamVisitor<void> {
		Interpreter& interpreter;
		InterpreterContext& ctxt;
		RamIndexScanKeysAnalysis* keysAnalysis;

	public:
		OperationEvaluator(Interpreter& interp, InterpreterContext& ctxt) :
				interpreter(interp), ctxt(ctxt), keysAnalysis(
						interp.getTranslationUnit().getAnalysis<
								RamIndexScanKeysAnalysis>()) {
		}

		// -- Operations -----------------------------

		void visitNestedOperation(const RamNestedOperation& nested) override {
			visit(nested.getOperation());
		}

		void visitSearch(const RamSearch& search) override {
			visitNestedOperation(search);

			if (Global::config().has("profile")
					&& !search.getProfileText().empty()) {
				interpreter.frequencies[search.getProfileText()][interpreter.getIterationNumber()]++;
			}
		}

		void visitScan(const RamScan& scan) override {
			//std::cout << "visitScan here!\n";
			// get the targeted relation
			const InterpreterRelation& rel = interpreter.getRelation(
					scan.getRelation());

			// use simple iterator
			for (const RamDomain* cur : rel) {
				ctxt[scan.getIdentifier()] = cur;
				visitSearch(scan);
			}
		}

		void visitIndexScan(const RamIndexScan& scan) override {
			//std::cout << "visitIndexScan here!\n";
			// get the targeted relation
			const InterpreterRelation& rel = interpreter.getRelation(
					scan.getRelation());

			// create pattern tuple for range query
			auto arity = rel.getArity();
			RamDomain low[arity];
			RamDomain hig[arity];
			auto pattern = scan.getRangePattern();
			for (size_t i = 0; i < arity; i++) {
				if (pattern[i] != nullptr) {
					low[i] = interpreter.evalVal(*pattern[i], ctxt);
					hig[i] = low[i];
				} else {
					low[i] = MIN_RAM_DOMAIN;
					hig[i] = MAX_RAM_DOMAIN;
				}
			}

			// obtain index
			auto idx = rel.getIndex(keysAnalysis->getRangeQueryColumns(&scan),
					nullptr);

			// get iterator range
			auto range = idx->lowerUpperBound(low, hig);

			// conduct range query
			for (auto ip = range.first; ip != range.second; ++ip) {
				const RamDomain* data = *(ip);
				//std::cout << "scan.getIdentifier():" <<scan.getIdentifier()<<"\n";
				ctxt[scan.getIdentifier()] = data;
				visitSearch(scan);
			}
		}

		void visitLookup(const RamLookup& lookup) override {
			// get reference
			RamDomain ref =
					ctxt[lookup.getReferenceLevel()][lookup.getReferencePosition()];

			// check for null
			if (isNull(ref)) {
				return;
			}

			// update environment variable
			auto arity = lookup.getArity();
			const RamDomain* tuple = unpack(ref, arity);

			// save reference to temporary value
			ctxt[lookup.getIdentifier()] = tuple;

			// run nested part - using base class visitor
			visitSearch(lookup);
		}

		void visitAggregate(const RamAggregate& aggregate) override {
			// get the targeted relation
			const InterpreterRelation& rel = interpreter.getRelation(
					aggregate.getRelation());

			// initialize result
			RamDomain res = 0;
			switch (aggregate.getFunction()) {
			case RamAggregate::MIN:
				res = MAX_RAM_DOMAIN;
				break;
			case RamAggregate::MAX:
				res = MIN_RAM_DOMAIN;
				break;
			case RamAggregate::COUNT:
				res = 0;
				break;
			case RamAggregate::SUM:
				res = 0;
				break;
			}

			// init temporary tuple for this level
			auto arity = rel.getArity();

			// get lower and upper boundaries for iteration
			const auto& pattern = aggregate.getPattern();
			RamDomain low[arity];
			RamDomain hig[arity];

			for (size_t i = 0; i < arity; i++) {
				if (pattern[i] != nullptr) {
					low[i] = interpreter.evalVal(*pattern[i], ctxt);
					hig[i] = low[i];
				} else {
					low[i] = MIN_RAM_DOMAIN;
					hig[i] = MAX_RAM_DOMAIN;
				}
			}

			// obtain index
			auto idx = rel.getIndex(aggregate.getRangeQueryColumns());

			// get iterator range
			auto range = idx->lowerUpperBound(low, hig);

			// check for emptiness
			if (aggregate.getFunction() != RamAggregate::COUNT) {
				if (range.first == range.second) {
					return;  // no elements => no min/max
				}
			}

			// iterate through values
			for (auto ip = range.first; ip != range.second; ++ip) {
				// link tuple
				const RamDomain* data = *(ip);
				ctxt[aggregate.getIdentifier()] = data;

				// count is easy
				if (aggregate.getFunction() == RamAggregate::COUNT) {
					++res;
					continue;
				}

				// aggregation is a bit more difficult

				// eval target expression
				RamDomain cur = interpreter.evalVal(
						*aggregate.getTargetExpression(), ctxt);

				switch (aggregate.getFunction()) {
				case RamAggregate::MIN:
					res = std::min(res, cur);
					break;
				case RamAggregate::MAX:
					res = std::max(res, cur);
					break;
				case RamAggregate::COUNT:
					res = 0;
					break;
				case RamAggregate::SUM:
					res += cur;
					break;
				}
			}

			// write result to environment
			RamDomain tuple[1];
			tuple[0] = res;
			ctxt[aggregate.getIdentifier()] = tuple;

			// run nested part - using base class visitor
			visitSearch(aggregate);
		}

		void visitFilter(const RamFilter& filter) override {
			//std::cout << "visitFilter here!\n";
			// check condition
			if (interpreter.evalCond(filter.getCondition(), ctxt)) {
				// process nested
				visitNestedOperation(filter);
			}

			if (Global::config().has("profile")
					&& !filter.getProfileText().empty()) {
				interpreter.frequencies[filter.getProfileText()][interpreter.getIterationNumber()]++;
			}
		}

		void visitProject(const RamProject& project) override {
			// create a tuple of the proper arity (also supports arity 0)
			//std::cout << "visitProject here.\n";
			auto arity = project.getRelation().getArity();
			const auto& values = project.getValues();
			RamDomain tuple[arity];
			for (size_t i = 0; i < arity; i++) {
				assert(values[i]);
				//std::cout << "visitProject -- evalVal here, arity = " << arity << ".\n";
				tuple[i] = interpreter.evalVal(*values[i], ctxt);
			}

			// insert in target relation
			InterpreterRelation& rel = interpreter.getRelation(
					project.getRelation());
			rel.insert(tuple);
		}

		// -- return from subroutine --
		void visitReturn(const RamReturn& ret) override {
			for (auto val : ret.getValues()) {
				if (val == nullptr) {
					ctxt.addReturnValue(0, true);
				} else {
					ctxt.addReturnValue(interpreter.evalVal(*val, ctxt));
				}
			}
		}

		// -- safety net --
		void visitNode(const RamNode& node) override {
			std::cerr << "Unsupported node type: " << typeid(node).name()
					<< "\n";
			assert(false && "Unsupported Node Type!");
		}
	};

	// create and run interpreter for operations
	InterpreterContext ctxt(
			translationUnit.getAnalysis<RamOperationDepthAnalysis>()->getDepth(
					&op));
	ctxt.setReturnValues(args.getReturnValues());
	ctxt.setReturnErrors(args.getReturnErrors());
	ctxt.setArguments(args.getArguments());
	OperationEvaluator(*this, ctxt).visit(op);
}

/** Evaluate RAM statement */
void Interpreter::evalStmt(const RamStatement& stmt) {
	class StatementEvaluator: public RamVisitor<bool> {
		Interpreter& interpreter;

	public:
		StatementEvaluator(Interpreter& interp) :
				interpreter(interp) {
		}

		// -- Statements -----------------------------

		bool visitSequence(const RamSequence& seq) override {
			// process all statements in sequence
			for (const auto& cur : seq.getStatements()) {
				if (!visit(cur)) {
					return false;
				}
			}

			// all processed successfully
			return true;
		}

		bool visitParallel(const RamParallel& parallel) override {
			// get statements to be processed in parallel
			const auto& stmts = parallel.getStatements();
//			std::cout << "PARALLEL size: " << stmts.size() << std::endl;
//			for (size_t i = 0; i < stmts.size(); i++) {
//				stmts[i]->print(std::cout);
//				std::cout << "\nEnd of a stmt.\n";
//			}
			// special case: empty
			if (stmts.empty()) {
				return true;
			}

			// special handling for a single child
			if (stmts.size() == 1) {
				return visit(stmts[0]);
			}

			// parallel execution
			bool cond = true;
#pragma omp parallel for reduction(&& : cond)
			for (size_t i = 0; i < stmts.size(); i++) {
				cond = cond && visit(stmts[i]);
			}
			return cond;
		}

		bool visitLoop(const RamLoop& loop) override {
			interpreter.resetIterationNumber();
			while (visit(loop.getBody())) {
				interpreter.incIterationNumber();
			}
			interpreter.resetIterationNumber();
			return true;
		}

		bool visitExit(const RamExit& exit) override {
			return !interpreter.evalCond(exit.getCondition());
		}

		bool visitLogTimer(const RamLogTimer& timer) override {
			if (timer.getRelation() == nullptr) {
				Logger logger(timer.getMessage().c_str(),
						interpreter.getIterationNumber());
				return visit(timer.getStatement());
			} else {
				const InterpreterRelation& rel = interpreter.getRelation(
						*timer.getRelation());
				Logger logger(timer.getMessage().c_str(),
						interpreter.getIterationNumber(),
						std::bind(&InterpreterRelation::size, &rel));
				return visit(timer.getStatement());
			}
		}

		bool visitDebugInfo(const RamDebugInfo& dbg) override {
			SignalHandler::instance()->setMsg(dbg.getMessage().c_str());
			return visit(dbg.getStatement());
		}

		bool visitStratum(const RamStratum& stratum) override {
			// TODO (lyndonhenry): should enable strata as subprograms for interpreter here
//			std::cout << "visitStratum!\n";
			// Record relations created in each stratum
			if (Global::config().has("profile")) {
				std::map<std::string, size_t> relNames;
				visitDepthFirst(stratum,
						[&](const RamCreate& create) {
							relNames[create.getRelation().getName()] = create.getRelation().getArity();
						});
				for (const auto& cur : relNames) {
					// Skip temporary relations, marked with '@'
					if (cur.first[0] == '@') {
						continue;
					}
					ProfileEventSingleton::instance().makeStratumRecord(
							stratum.getIndex(), "relation", cur.first, "arity",
							std::to_string(cur.second));
				}
			}
			return visit(stratum.getBody());
		}

		bool visitCreate(const RamCreate& create) override {
			interpreter.createRelation(create.getRelation());
			return true;
		}

		bool visitClear(const RamClear& clear) override {
			InterpreterRelation& rel = interpreter.getRelation(
					clear.getRelation());
			rel.purge();
			return true;
		}

		bool visitDrop(const RamDrop& drop) override {
			interpreter.dropRelation(drop.getRelation());
			return true;
		}

		bool visitLogSize(const RamLogSize& size) override {
			const InterpreterRelation& rel = interpreter.getRelation(
					size.getRelation());
			ProfileEventSingleton::instance().makeQuantityEvent(
					size.getMessage(), rel.size(),
					interpreter.getIterationNumber());
			return true;
		}

		bool visitLoad(const RamLoad& load) override {
			for (IODirectives ioDirectives : load.getIODirectives()) {
				try {
					InterpreterRelation& relation = interpreter.getRelation(
							load.getRelation());
					IOSystem::getInstance().getReader(
							load.getRelation().getSymbolMask(),
							load.getRelation().getEnumTypeMask(),
							interpreter.getSymbolTable(), ioDirectives,
							Global::config().has("provenance"))->readAll(
							relation);
				} catch (std::exception& e) {
					std::cout << "symbolmask:\n";
					load.getRelation().getSymbolMask().print(std::cout);
					std::cout << "\n";
					std::cout << "symboltable:\n";
					interpreter.getSymbolTable().print(std::cout);
					std::cout << "\n";
					std::cerr << "Error loading data: " << e.what() << "\n";
				}
			}
			return true;
		}
		bool visitStore(const RamStore& store) override {
//			std::cout << "start visitStore\n";
			for (IODirectives ioDirectives : store.getIODirectives()) {
				try {
					IOSystem::getInstance().getWriter(
							store.getRelation().getSymbolMask(),
							store.getRelation().getEnumTypeMask(),
							interpreter.getSymbolTable(), ioDirectives,
							Global::config().has("provenance"))->writeAll(
							interpreter.getRelation(store.getRelation()));
				} catch (std::exception& e) {
					std::cerr << e.what();
					exit(1);
				}
			}
//			std::cout << "finish visitStore\n";
			return true;
		}

		bool visitFact(const RamFact& fact) override {
			auto arity = fact.getRelation().getArity();
			RamDomain tuple[arity];
			auto values = fact.getValues();

			for (size_t i = 0; i < arity; ++i) {
				tuple[i] = interpreter.evalVal(*values[i]);
			}

			interpreter.getRelation(fact.getRelation()).insert(tuple);
			return true;
		}

		bool visitInsert(const RamInsert& insert) override {
			// run generic query executor
			//std::cout << "visitInsert here.\n";
			const RamCondition* c = insert.getCondition();
			if (c != nullptr) {
				if (interpreter.evalCond(*insert.getCondition())) {
					interpreter.evalOp(insert.getOperation());
				}
			} else {
				interpreter.evalOp(insert.getOperation());
			}
			//std::cout << "visitInsert finish.\n";
			return true;
		}

		bool visitMerge(const RamMerge& merge) override {
			// get involved relation
			InterpreterRelation& src = interpreter.getRelation(
					merge.getSourceRelation());
			InterpreterRelation& trg = interpreter.getRelation(
					merge.getTargetRelation());

			if (dynamic_cast<InterpreterEqRelation*>(&trg)) {
				// expand src with the new knowledge generated by insertion.
				src.extend(trg);
			}
			// merge in all elements
			trg.insert(src);
			//std::cout << "visitMerge finish.\n";
			// done
			return true;
		}

		bool visitLatNorm(const RamLatNorm& latnorm) override {
//			std::cout << "\n visit LatNorm here! relation: "
//					<< latnorm.getRelation_IN_Rel().getName() << std::endl;
			RamDomain lat_Top =
					interpreter.getTranslationUnit().getProgram()->getLattice()->getTop();
			InterpreterRelation& IN_Rel = interpreter.getRelation(
					latnorm.getRelation_IN_Rel());
			InterpreterRelation& OUT_Rel = interpreter.getRelation(
					latnorm.getRelation_OUT_Rel());
			//TODO
			size_t arity = IN_Rel.getArity();
			RamDomain low[arity];
			RamDomain high[arity];
			for (size_t i = 0; i < arity; i++) {
				low[i] = MIN_RAM_DOMAIN;
				high[i] = MAX_RAM_DOMAIN;
			}

			InterpreterContext ctxt_temp;
			const RamLatticeBinaryFunction& lub_func =
					interpreter.getTranslationUnit().getProgram()->getLattice()->getLUB();

			// obtain index
			auto totalIndex = IN_Rel.getTotalIndex();

			auto range = totalIndex->lowerUpperBound(low, high);
			auto it = range.first;
			auto itend = range.second;
			while (it != itend) {
				const RamDomain* data = *(it);
				for (size_t i = 0; i < arity - 1; i++) {
					high[i] = data[i];
				}
				high[arity - 1] = MAX_RAM_DOMAIN; // must keep this

				// get iterator range
				auto range_end = totalIndex->UpperBound(high);
				//			RamDomain biggestLat = latAssoc->getBot();
				RamDomain biggestLat = data[arity - 1];
				++it;
				for (; it != range_end; ++it) {
					// skip if obtain the top element
					if (biggestLat == lat_Top) {
						it = range_end;
						break;
					}

					RamDomain curlat = (*it)[arity - 1];
					// set 2 input variables
					std::vector<RamDomain> args = { biggestLat, curlat };
					ctxt_temp.setArguments(args);

					// evaluate binary constraint
					for (const auto& cas : lub_func.getLatCase()) {
						if (cas.match == nullptr
								|| interpreter.evalCond(*cas.match,
										ctxt_temp)) {
							// get output if match
							biggestLat = interpreter.evalVal(*cas.output,
									ctxt_temp);
							break;
						}
					}
					//				biggestLat = latAssoc->applyLub(biggestLat, curlat);
				}
				high[arity - 1] = biggestLat;

				OUT_Rel.insert(high);
			}

			return true;
		}

		// Plan B
		bool visitLatClean(const RamLatClean& latclean) override {
//			std::cout << "\n visit LatClean here! relation: "
//					<< latclean.getRelation_IN_Origin().getName() << std::endl;
			RamDomain lat_Top =
					interpreter.getTranslationUnit().getProgram()->getLattice()->getTop();
			// get involved relation
			InterpreterRelation& IN_Origin = interpreter.getRelation(
					latclean.getRelation_IN_Origin());
			InterpreterRelation& IN_New = interpreter.getRelation(
					latclean.getRelation_IN_New());
			InterpreterRelation& OUT_New = interpreter.getRelation(
					latclean.getRelation_OUT_New());

			size_t arity = IN_Origin.getArity();

			// Notice: there could be several elements for each cell in in_origin lattice relation!
			// Find biggest lattice element in the same cell. If the biggest element does not
			// exist in in_origin, then put it in out_new
			RamDomain low[arity];
			RamDomain high[arity];
			for (size_t i = 0; i < arity; i++) {
				low[i] = MIN_RAM_DOMAIN;
				high[i] = MAX_RAM_DOMAIN;
			}

			InterpreterContext ctxt_temp;
			const RamLatticeBinaryFunction& lub_func =
					interpreter.getTranslationUnit().getProgram()->getLattice()->getLUB();

			// obtain index
			auto org_totalIndex = IN_Origin.getTotalIndex();
			auto new_totalIndex = IN_New.getTotalIndex();

			auto new_range = new_totalIndex->lowerUpperBound(low, high);
			auto new_it = new_range.first;
			auto new_itend = new_range.second;

			// Traverse the whole new lattice relation
			while (new_it != new_itend) {
				const RamDomain* data = *new_it;
				RamDomain biggestLat = data[arity - 1];
				//				std::cout << "data: ";
				//				for (size_t i = 0; i < arity; i++) {
				//					std::cout << data[i] << " ";
				//				}
				//				std::cout << std::endl;

				for (size_t i = 0; i < arity - 1; i++) {
					low[i] = data[i];
					high[i] = data[i];
				}
				low[arity - 1] = MIN_RAM_DOMAIN;
				high[arity - 1] = MAX_RAM_DOMAIN; // must keep this
				auto cell_new_end = new_totalIndex->UpperBound(high);

				// 1. traverse the new lattice relation
				for (++new_it; new_it != cell_new_end; ++new_it) {
					// skip if obtain the top element
					if (biggestLat == lat_Top) {
						new_it = cell_new_end;
						break;
					}

					RamDomain curlat = (*new_it)[arity - 1];
					// set 2 input variables
					std::vector<RamDomain> ctxt_args = { biggestLat, curlat };
					ctxt_temp.setArguments(ctxt_args);
					// evaluate binary constraint
					for (const auto& cas : lub_func.getLatCase()) {
						if (cas.match == nullptr
								|| interpreter.evalCond(*cas.match,
										ctxt_temp)) {
							// get output if match
							biggestLat = interpreter.evalVal(*cas.output,
									ctxt_temp);
							break;
						}
					}
				}


				// 2. traverse the cell in the original lattice relation
				if (biggestLat != lat_Top) {
					auto cell_org_range = org_totalIndex->lowerUpperBound(low,
							high);
					auto cell_org_it = cell_org_range.first;
					auto cell_org_end = cell_org_range.second;
					for (; cell_org_it != cell_org_end; ++cell_org_it) {
						// skip if obtain the top element
						if (biggestLat == lat_Top) {
							break;
						}

						RamDomain curlat = (*cell_org_it)[arity - 1];
						// set 2 input variables
						std::vector<RamDomain> ctxt_args =
								{ biggestLat, curlat };
						ctxt_temp.setArguments(ctxt_args);
						// evaluate binary constraint
						for (const auto& cas : lub_func.getLatCase()) {
							if (cas.match == nullptr
									|| interpreter.evalCond(*cas.match,
											ctxt_temp)) {

								// get output if match
								biggestLat = interpreter.evalVal(*cas.output,
										ctxt_temp);
								break;
							}
						}
					}
				}

				// 3. put element into temp new relation
				high[arity - 1] = biggestLat;
				if (!IN_Origin.exists(high)) {
					OUT_New.insert(high);
				}

			}

			return true;
		}

		// Plan A TODO, use RamDomain lat_Top to optimize
//		bool visitLatExt(const RamLatExt& latext) override {
////			std::cout << "\n visit LatExt here! relation: " << latext.getRelation_IN_Origin().getName() << std::endl;
//			// get involved relation
//			InterpreterRelation& IN_Origin = interpreter.getRelation(
//					latext.getRelation_IN_Origin());
//			InterpreterRelation& IN_New = interpreter.getRelation(
//					latext.getRelation_IN_New());
//			InterpreterRelation& OUT_Origin = interpreter.getRelation(
//					latext.getRelation_OUT_Origin());
//			InterpreterRelation& OUT_New = interpreter.getRelation(
//					latext.getRelation_OUT_New());
//
//			size_t arity = IN_Origin.getArity();
//
//			// Require: there is only one element for each cell in in_origin lattice relation!
//			// Find biggest lattice element in the same cell, and
//			// put it into out_origin. If the biggest element does not
//			// exist in in_origin, also put it in out_new
//			RamDomain low[arity];
//			RamDomain high[arity];
//			for (size_t i = 0; i < arity; i++) {
//				low[i] = MIN_RAM_DOMAIN;
//				high[i] = MAX_RAM_DOMAIN;
//			}
//
//			InterpreterContext ctxt_temp;
//			const RamLatticeBinaryFunction& lub_func =
//					interpreter.getTranslationUnit().getProgram()->getLattice()->getLUB();
//
//			// obtain index
//			auto org_totalIndex = IN_Origin.getTotalIndex();
//			auto new_totalIndex = IN_New.getTotalIndex();
//
//			auto org_range = org_totalIndex->lowerUpperBound(low, high);
//			auto org_it = org_range.first;
//			auto org_itend = org_range.second;
//
////			std::cout << "Step 1\n";
//			// Step 1: Traverse the whole original relation, require that only 1 element in 1 cell!
//			for (; org_it != org_itend; ++org_it) {
//				const RamDomain* data = *org_it;
//
////				std::cout << "data: ";
////				for (size_t i = 0; i < arity; i++) {
////					std::cout << data[i] << " ";
////				}
////				std::cout << std::endl;
//
//				for (size_t i = 0; i < arity - 1; i++) {
//					low[i] = data[i];
//					high[i] = data[i];
//				}
//				low[arity - 1] = MIN_RAM_DOMAIN;
//				high[arity - 1] = MAX_RAM_DOMAIN; // must keep this
//
//				auto new_range = new_totalIndex->lowerUpperBound(low, high);
//				auto new_it = new_range.first;
//				auto new_itend = new_range.second;
//
//				if (new_it != new_itend) {
//					// the cell exists in the new lattice relation
//					RamDomain Org_Lat = data[arity - 1];
//					RamDomain biggestLat = Org_Lat;
//
//					// search the cell in new lattice relation
//					for (; new_it != new_itend; ++new_it) {
//						RamDomain curlat = (*new_it)[arity - 1];
//						// set 2 input variables
//						std::vector<RamDomain> ctxt_args =
//								{ biggestLat, curlat };
//						ctxt_temp.setArguments(ctxt_args);
//						// evaluate binary constraint
//						for (const auto& cas : lub_func.getLatCase()) {
//							if (cas.match == nullptr
//									|| interpreter.evalCond(*cas.match,
//											ctxt_temp)) {
//								// get output if match
//								biggestLat = interpreter.evalVal(*cas.output,
//										ctxt_temp);
//								break;
//							}
//						}
//					}
//					high[arity - 1] = biggestLat;
////					std::cout << "biggestLat: " << biggestLat << " Org_Lat:" << Org_Lat << std::endl;
//					OUT_Origin.insert(high);
//					if (biggestLat != Org_Lat) {
//						OUT_New.insert(high);
//					}
//
////					std::cout << "insert - exist in new: ";
////					if (biggestLat != Org_Lat) {
////						std::cout << "(both) ";
////					}
////					for (size_t i = 0; i < arity; i++) {
////						std::cout << high[i] << " ";
////					}
////					std::cout << std::endl;
//
//				} else {
//					// the cell does not exist in the new lattice relation
//					OUT_Origin.insert(data);
//
////					std::cout << "insert - not exist in new: ";
////					for (size_t i = 0; i < arity; i++) {
////						std::cout << data[i] << " ";
////					}
////					std::cout << std::endl;
//				}
//
//			}
//
//			// Step 2: Traverse the whole new lattice relation
//
////			std::cout << "Step 2\n";
//			for (size_t i = 0; i < arity; i++) {
//				low[i] = MIN_RAM_DOMAIN;
//				high[i] = MAX_RAM_DOMAIN;
//			}
//			auto new_range = new_totalIndex->lowerUpperBound(low, high);
//			auto new_it = new_range.first;
//			auto new_itend = new_range.second;
//			while (new_it != new_itend) {
//				const RamDomain* data = *new_it;
//
////				std::cout << "data: ";
////				for (size_t i = 0; i < arity; i++) {
////					std::cout << data[i] << " ";
////				}
////				std::cout << std::endl;
//
//				for (size_t i = 0; i < arity - 1; i++) {
//					low[i] = data[i];
//					high[i] = data[i];
//				}
//				low[arity - 1] = MIN_RAM_DOMAIN;
//				high[arity - 1] = MAX_RAM_DOMAIN; // must keep this
//				auto range_end = new_totalIndex->UpperBound(high);
//
//				if (org_totalIndex->LowerBound(low)
//						== org_totalIndex->UpperBound(high)) {
//					// the cell does not exist in original lattice relation, need to handle
////					std::cout
////							<< "the cell does not exist in original lattice relation\n";
//					RamDomain biggestLat = data[arity - 1];
//					for (++new_it; new_it != range_end; ++new_it) {
//						RamDomain curlat = (*new_it)[arity - 1];
//						// set 2 input variables
//						std::vector<RamDomain> ctxt_args =
//								{ biggestLat, curlat };
//						ctxt_temp.setArguments(ctxt_args);
//						// evaluate binary constraint
//						for (const auto& cas : lub_func.getLatCase()) {
//							if (cas.match == nullptr
//									|| interpreter.evalCond(*cas.match,
//											ctxt_temp)) {
//								// get output if match
//								biggestLat = interpreter.evalVal(*cas.output,
//										ctxt_temp);
//								break;
//							}
//						}
//					}
//					high[arity - 1] = biggestLat;
//					OUT_Origin.insert(high);
//					OUT_New.insert(high);
//
////					std::cout << "insert both: ";
////					for (size_t i = 0; i < arity; i++) {
////						std::cout << high[i] << " ";
////					}
////					std::cout << std::endl;
//
//				} else {
//					// the cell exists in original lattice relation, already handled
//					new_it = range_end;
//				}
//			}
//
//			return true;
//		}

		bool visitSwap(const RamSwap& swap) override {
			interpreter.swapRelation(swap.getFirstRelation(),
					swap.getSecondRelation());
			return true;
		}

		// -- safety net --

		bool visitNode(const RamNode& node) override {
			auto lease = getOutputLock().acquire();
			(void) lease;
			std::cerr << "Unsupported node type: " << typeid(node).name()
					<< "\n";
			assert(false && "Unsupported Node Type!");
			return false;
		}
	};

	// create and run interpreter for statements
	StatementEvaluator(*this).visit(stmt);
}

/** Execute main program of a translation unit */
void Interpreter::executeMain() {
	SignalHandler::instance()->set();
	if (Global::config().has("verbose")) {
		SignalHandler::instance()->enableLogging();
	}
	const RamStatement& main = *translationUnit.getP().getMain();

	if (!Global::config().has("profile")) {
		evalStmt(main);
	} else {
		ProfileEventSingleton::instance().setOutputFile(
				Global::config().get("profile"));
		// Prepare the frequency table for threaded use
		visitDepthFirst(main,
				[&](const RamSearch& node) {
					if (!node.getProfileText().empty()) {
						frequencies.emplace(node.getProfileText(), std::map<size_t, size_t>());
					}
				});
		// Enable profiling for execution of main
		ProfileEventSingleton::instance().startTimer();
		ProfileEventSingleton::instance().makeTimeEvent("@time;starttime");
		// Store configuration
		for (const auto& cur : Global::config().data()) {
			ProfileEventSingleton::instance().makeConfigRecord(cur.first,
					cur.second);
		}
		// Store count of relations
		size_t relationCount = 0;
		visitDepthFirst(main, [&](const RamCreate& create) {
			if (create.getRelation().getName()[0] != '@') {
				++relationCount;
				reads[create.getRelation().getName()] = 0;
			}
		});
		ProfileEventSingleton::instance().makeConfigRecord("relationCount",
				std::to_string(relationCount));

		// Store count of rules
		size_t ruleCount = 0;
		visitDepthFirst(main, [&](const RamInsert& rule) {++ruleCount;});
		ProfileEventSingleton::instance().makeConfigRecord("ruleCount",
				std::to_string(ruleCount));

		evalStmt(main);

		ProfileEventSingleton::instance().stopTimer();
		for (auto const& cur : frequencies) {
			for (auto const& iter : cur.second) {
				ProfileEventSingleton::instance().makeQuantityEvent(cur.first,
						iter.second, iter.first);
			}
		}
		for (auto const& cur : reads) {
			ProfileEventSingleton::instance().makeQuantityEvent(
					"@relation-reads;" + cur.first, cur.second, 0);
		}
	}
	SignalHandler::instance()->reset();
}

/** Execute subroutine */
void Interpreter::executeSubroutine(const RamStatement& stmt,
		const std::vector<RamDomain>& arguments,
		std::vector<RamDomain>& returnValues, std::vector<bool>& returnErrors) {
	InterpreterContext ctxt;
	ctxt.setReturnValues(returnValues);
	ctxt.setReturnErrors(returnErrors);
	ctxt.setArguments(arguments);

	// run subroutine
	const RamOperation& op = static_cast<const RamInsert&>(stmt).getOperation();
	evalOp(op, ctxt);
}

}  // end of namespace souffle
