/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RamNode.h
 *
 * Top level syntactic element of intermediate representation,
 * i.e., a node of the RAM machine code.
 *
 ***********************************************************************/

#pragma once

#include <cassert>
#include <iostream>
#include <memory>
#include <typeinfo>
#include <vector>

namespace souffle {

class RamNodeMapper;

enum RamNodeType {
    // Relations
    RN_Relation,
    RN_RelationReference,

	// Lattice Association and Binary functions
	RN_LatticeAssociation,
	RN_LatticeUnaryFunction,
	RN_LatticeBinaryFunction,

    // Values
    RN_ElementAccess,
	RN_LatticeGLB,
    RN_Number,
    RN_IntrinsicOperator,
    RN_UserDefinedOperator,
	RN_LatticeUnaryFunctor,
	RN_LatticeBinaryFunctor,
    RN_AutoIncrement,
    RN_Pack,
    RN_Argument,
	RN_QuestionMark,

    // Conditions
    RN_ExistenceCheck,
    RN_ProvenanceExistenceCheck,
    RN_EmptinessCheck,
    RN_Conjunction,
    RN_Negation,
    RN_Constraint,

    // Operations
    RN_Project,
    RN_Lookup,
    RN_Scan,
    RN_IndexScan,
    RN_Aggregate,
    RN_Filter,

    // Statements
    RN_Create,
    RN_Fact,
    RN_Load,
    RN_Store,
    RN_Insert,
    RN_Clear,
    RN_Drop,
    RN_LogSize,
    RN_Return,

    RN_Merge,
	RN_LatNorm,
	RN_LatClean,
//	RN_LatExt,
    RN_Swap,

    // Control-flow
    RN_Program,
    RN_Sequence,
    RN_Loop,
    RN_Parallel,
    RN_Exit,
    RN_LogTimer,
    RN_DebugInfo,
    RN_Stratum

#ifdef USE_MPI
    // mpi
    ,
    RN_Send,
    RN_Recv,
    RN_Notify,
    RN_Wait,
#endif
};

/**
 *  @class RamNode
 *  @brief RamNode is a superclass for all RAM IR classes.
 */
class RamNode {
    const RamNodeType type;

public:
    RamNode(RamNodeType type) : type(type) {}

    /** A virtual destructor for RAM nodes */
    virtual ~RamNode() = default;

    /** Get type of node */
    RamNodeType getNodeType() const {
        return type;
    }

    /** Equivalence check for two RAM nodes */
    bool operator==(const RamNode& other) const {
        return this == &other || (typeid(*this) == typeid(other) && equal(other));
    }

    /** Inequality check for two RAM nodes */
    bool operator!=(const RamNode& other) const {
        return !(*this == other);
    }

    /** Create a clone (i.e. deep copy) of this node */
    virtual RamNode* clone() const = 0;

    /** Apply the mapper to all child nodes */
    virtual void apply(const RamNodeMapper& mapper) = 0;

    /** Obtain list of all embedded child nodes */
    virtual std::vector<const RamNode*> getChildNodes() const = 0;

    /** Print RAM node */
    virtual void print(std::ostream& out = std::cout) const = 0;

    /** Print RAM on a stream */
    friend std::ostream& operator<<(std::ostream& out, const RamNode& node) {
        node.print(out);
        return out;
    }

protected:
    /** Abstract equality check for two RAM nodes */
    virtual bool equal(const RamNode& other) const = 0;
};

/**
 * An abstract class for manipulating RAM Nodes by substitution
 */
class RamNodeMapper {
public:
    virtual ~RamNodeMapper() = default;

    /**
     * Abstract replacement method for a node.
     *
     * If the given nodes is to be replaced, the handed in node
     * will be destroyed by the mapper and the returned node
     * will become owned by the caller.
     */
    virtual std::unique_ptr<RamNode> operator()(std::unique_ptr<RamNode> node) const = 0;

    /**
     * Wrapper for any subclass of the RAM node hierarchy performing type casts.
     */
    template <typename T>
    std::unique_ptr<T> operator()(std::unique_ptr<T> node) const {
        std::unique_ptr<RamNode> resPtr =
                (*this)(std::unique_ptr<RamNode>(static_cast<RamNode*>(node.release())));
        assert(nullptr != dynamic_cast<T*>(resPtr.get()) && "Invalid target node!");
        return std::unique_ptr<T>(dynamic_cast<T*>(resPtr.release()));
    }
};

namespace detail {

/**
 * A special RamNodeMapper wrapping a lambda conducting node transformations.
 */
template <typename Lambda>
class LambdaRamNodeMapper : public RamNodeMapper {
    const Lambda& lambda;

public:
    LambdaRamNodeMapper(const Lambda& lambda) : lambda(lambda) {}

    std::unique_ptr<RamNode> operator()(std::unique_ptr<RamNode> node) const override {
        return lambda(std::move(node));
    }
};
}  // namespace detail

/**
 * Creates a node mapper based on a corresponding lambda expression.
 */
template <typename Lambda>
detail::LambdaRamNodeMapper<Lambda> makeLambdaMapper(const Lambda& lambda) {
    return detail::LambdaRamNodeMapper<Lambda>(lambda);
}

}  // end of namespace souffle
