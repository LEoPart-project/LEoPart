// Author: Jakob Maljaars
// Contact: j.m.maljaars _at_ tudelft.nl/jakobmaljaars _at_ gmail.com

#include "pdestaticcondensation.h"

using namespace dolfin;

PDEStaticCondensation::PDEStaticCondensation(const Mesh& mesh, particles& P,
                                             const Form& N, const Form& G, const Form& L,
                                             const Form& H, const Form& B,
                                             const Form& Q, const Form& R, const Form& S,
                                             const std::size_t idx_pproperty)
    : mesh(&mesh), _P(&P), N(&N), G(&G), L(&L), H(&H), B(&B), Q(&Q), R(&R), S(&S), mpi_comm(mesh.mpi_comm()),
      invKS_list(mesh.num_cells()), LHe_list(mesh.num_cells()), Ge_list(mesh.num_cells()), Be_list(mesh.num_cells()),
      Re_list(mesh.num_cells()), QRe_list(mesh.num_cells()), _idx_pproperty(idx_pproperty)
{
    FormUtils::test_rank(*(this->N),2);
    FormUtils::test_rank(*(this->G),2);
    FormUtils::test_rank(*(this->L),2);
    FormUtils::test_rank(*(this->H),2);
    FormUtils::test_rank(*(this->B),2);
    FormUtils::test_rank(*(this->Q),1);
    FormUtils::test_rank(*(this->R),1);
    FormUtils::test_rank(*(this->S),1);

    // Initialize matrix and vector with proper sparsity structures
    AssemblerBase assembler_base;
    assembler_base.init_global_tensor(A_g, *(this->B));
    assembler_base.init_global_tensor(f_g, *(this->S));

    // TODO: Put an assertion here: we need to have a DG function space at the moment
     _element = this->N->function_space(0)->element();

    _num_subspaces   = _element->num_sub_elements();
    _space_dimension = _element->space_dimension();
    if(_num_subspaces == 0){
         _num_dof_locs    = _space_dimension;
    }else{
        _num_dof_locs    = _space_dimension/_num_subspaces;
    }

    _value_size_loc = 1;
    for (std::size_t i = 0; i < _element->value_rank(); i++)
       _value_size_loc *= _element->value_dimension(i);

    if(_value_size_loc != _P->_ptemplate[_idx_pproperty])
        dolfin_error("l2projection","set _value_size_loc",
                     "Local value size (%d) mismatches particle template property with size (%d)",
                     _value_size_loc, _P->_ptemplate[_idx_pproperty]);
}
//-----------------------------------------------------------------------------
PDEStaticCondensation::PDEStaticCondensation(const Mesh& mesh, particles& P,
                                             const Form& N, const Form& G, const Form& L,
                                             const Form& H, const Form& B,
                                             const Form& Q, const Form& R, const Form& S,
                                             std::vector<std::shared_ptr<const DirichletBC>> bcs,
                                             const std::size_t idx_pproperty)
    : PDEStaticCondensation::PDEStaticCondensation(mesh, P, N, G, L, H, B, Q, R, S, idx_pproperty)
{
    this->bcs = bcs;
}
//-----------------------------------------------------------------------------
PDEStaticCondensation::~PDEStaticCondensation()
{
}
//-----------------------------------------------------------------------------
void PDEStaticCondensation::assemble(const bool assemble_all, const bool assemble_on_config){
    A_g.zero();
    f_g.zero();

    bool active_bcs = (!bcs.empty());

    // Collect bcs info, see dolfin::SystemAssembler
    std::vector<DirichletBC::Map> boundary_values(1);
    if (active_bcs){
        // Bin boundary conditions according to which form they apply to (if any)
          for (std::size_t i = 0; i < bcs.size(); ++i)
          {
            bcs[i]->get_boundary_values(boundary_values[0]);
            if (MPI::size(mpi_comm) > 1 && bcs[i]->method() != "pointwise")
                bcs[i]->gather(boundary_values[0]);
          }
    }

    for( CellIterator cell(*(this->mesh)); !cell.end(); ++cell){
        std::size_t nrowsN, ncolsN, nrowsG, ncolsG,
                    nrowsL, ncolsL, nrowsH, ncolsH,
                    nrowsB, ncolsB;
        ArrayView<const dolfin::la_index> cdof_rowsN, cdof_colsN, cdof_rowsG, cdof_colsG,
                                          cdof_rowsL, cdof_colsL, cdof_rowsH, cdof_colsH,
                                          cdof_rowsB, cdof_colsB;

        // Get local tensor info
        // TODO: We may not need all the info...
        FormUtils::local_tensor_info(*(this->N), *cell, &nrowsN, cdof_rowsN,
                                             &ncolsN, cdof_colsN);
        FormUtils::local_tensor_info(*(this->G), *cell, &nrowsG, cdof_rowsG,
                                             &ncolsG, cdof_colsG);
        FormUtils::local_tensor_info(*(this->L), *cell, &nrowsL, cdof_rowsL,
                                             &ncolsL, cdof_colsL);
        FormUtils::local_tensor_info(*(this->H), *cell, &nrowsH, cdof_rowsH,
                                             &ncolsH, cdof_colsH);
        FormUtils::local_tensor_info(*(this->B), *cell, &nrowsB, cdof_rowsB,
                                             &ncolsB, cdof_colsB);
        // Then do all the work     
        if(assemble_all){
            Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> G_e, L_e,
                                                                                   H_e, B_e;
            // CONSIDER TO REPLACE local_assembly of G --> non-linear problems
            FormUtils::local_assembler(G_e, *(this->G), *cell, nrowsG, ncolsG);
            FormUtils::local_assembler(L_e, *(this->L), *cell, nrowsL, ncolsL);
            FormUtils::local_assembler(H_e, *(this->H), *cell, nrowsH, ncolsH);
            FormUtils::local_assembler(B_e, *(this->B), *cell, nrowsB, ncolsB);

            Eigen::MatrixXd LH(nrowsN + nrowsH, ncolsB);
            LH << L_e , H_e;
            LHe_list[cell->index()]   = LH;
            Ge_list[cell->index()]    = G_e;
            Be_list[cell->index()]    = B_e;
        }

        // Particle contributions
        Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> q;
        Eigen::Matrix<double, Eigen::Dynamic, 1> f;

        // Matrices
        Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> N_e, N_ep;
        Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> Q_e, Q_ep, R_e, S_e;

        // The LHS matrix, can also be stored separately
        FormUtils::local_assembler(N_e, *(this->N), *cell, nrowsN, ncolsN);

        // The RHS, maybe check if form is zero, if so, we can skip assembly
        FormUtils::local_assembler(Q_e, *(this->Q), *cell, nrowsN, 1);

        // On moving meshes we even need to reassemble the R form (on new configuration)
        if(assemble_on_config){
             FormUtils::local_assembler(R_e, *(this->R), *cell, nrowsH, 1);
        }else{
             R_e = Re_list[cell->index()];
        }

        FormUtils::local_assembler(S_e, *(this->S), *cell, nrowsB, 1);

        _P->get_particle_contributions(q, f, *cell, _element,
                                       _space_dimension, _value_size_loc, _idx_pproperty);

        N_ep = q * q.transpose();
        Q_ep = q * f;

        Eigen::MatrixXd KS(nrowsN + ncolsG, nrowsN + ncolsG);
        Eigen::VectorXd QR(nrowsN + nrowsH, 1);
        Eigen::MatrixXd KS_zero(ncolsG, ncolsG);
        KS_zero.Zero(ncolsG, ncolsG);

        KS << N_e + N_ep, Ge_list[cell->index()], Ge_list[cell->index()].transpose(), Eigen::MatrixXd::Zero(ncolsG,ncolsG);
        QR << Q_e + Q_ep, R_e;

        // Compute inverse
        Eigen::MatrixXd invKS = KS.inverse();

        // Do some tests
        if( invKS.hasNaN() ) dolfin_error("KS", "not invertible", "not invertible");
        if (invKS.rows() != invKS.cols() ) warning("Wrong shape of invKS");
        if (LHe_list[cell->index()].rows() != invKS.rows()) warning("Wrong shape in multiplication");
        if (LHe_list[cell->index()].cols() != Be_list[cell->index()].rows() ) warning("Wrong shape in subtraction");
        if (Be_list[cell->index()].cols() != Be_list[cell->index()].rows() ) warning("Be not square");
        if (Q_ep.rows() != Q_e.rows()) warning("Wrong shape of Q_e");

        // Local contributions to be inserted in global matrix
        Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> LHS_e;
        Eigen::Matrix<double, Eigen::Dynamic, 1> RHS_e;

        LHS_e = LHe_list[cell->index()].transpose() * invKS * LHe_list[cell->index()] - Be_list[cell->index()];
        RHS_e = -S_e + LHe_list[cell->index()].transpose() * invKS * QR;

        // Apply BC's here (maintaining symmetry)
        if (active_bcs){
            FormUtils::apply_boundary_symmetric(LHS_e, RHS_e, cdof_rowsB, cdof_colsB,
                                     boundary_values, active_bcs);
        }

        A_g.add_local(LHS_e.data(), nrowsB, cdof_rowsB.data(), ncolsB, cdof_colsB.data());
        f_g.add_local(RHS_e.data(), nrowsB, cdof_rowsB.data());

        // Add to lists
        // TODO: if relevant
        invKS_list[cell->index()] = invKS;
        QRe_list[cell->index()]   = QR;
    }
    // Finalize assembly
    A_g.apply("add");
    f_g.apply("add");
}
//-----------------------------------------------------------------------------
void PDEStaticCondensation::assemble_state_rhs(){
    for( CellIterator cell(*(this->mesh)); !cell.end(); ++cell){
        std::size_t nrowsH, ncolsH;
        ArrayView<const dolfin::la_index> cdof_rowsH, cdof_colsH;

        FormUtils::local_tensor_info(*(this->H), *cell, &nrowsH, cdof_rowsH,
                                             &ncolsH, cdof_colsH);
        Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> R_e;
        FormUtils::local_assembler(R_e, *(this->R), *cell, nrowsH, 1);
        Re_list[cell->index()] = R_e;
    }
}
//-----------------------------------------------------------------------------
void PDEStaticCondensation::solve_problem(Function& Uglobal, Function& Ulocal,
                   const std::string solver, const std::string preconditioner){

    // TODO: Check if Uglobal, Ulocal are correct
    if( solver == "none" || solver == "mumps" || solver == "petsc"){
        // Direct solver
        solve(A_g, *(Uglobal.vector()), f_g);
    }else{
        // Iterative solver
        std::size_t num_it = solve(A_g, *(Uglobal.vector()), f_g, solver, preconditioner);
        if(MPI::rank(mpi_comm) == 0) std::cout<<"Number of iterations"<<num_it<<std::endl;
    }
    // Backsubtitution in Ulocal, check this carefully!
    backsubtitute(Uglobal, Ulocal);
}
//-----------------------------------------------------------------------------
// Return Lagrange multiplier also
void PDEStaticCondensation::solve_problem(Function& Uglobal, Function& Ulocal, Function& Lambda,
                   const std::string solver, const std::string preconditioner){

    // TODO: Check if Uglobal, Ulocal are correct
    if( solver == "none" ){
        // Direct solver
        solve(A_g, *(Uglobal.vector()), f_g);
    }else{
        // Iterative solver
        std::size_t num_it = solve(A_g, *(Uglobal.vector()), f_g, solver, preconditioner);
        if(MPI::rank(mpi_comm) == 0) std::cout<<"Number of iterations"<<num_it<<std::endl;
    }
    // Backsubtitution in Ulocal, check this carefully!
    backsubtitute(Uglobal, Ulocal, Lambda);
}
//-----------------------------------------------------------------------------
void PDEStaticCondensation::apply_boundary(DirichletBC& DBC){
    DBC.apply(A_g, f_g);
}
//-----------------------------------------------------------------------------
void PDEStaticCondensation::backsubtitute(const Function &Uglobal, Function &Ulocal){
    for( CellIterator cell(*(this->mesh)); !cell.end(); ++cell){
        // Backsubstitute global solution Uglobal to get local solution Ulocal
        std::size_t nrowsQ, ncolsQ, nrowsS, ncolsS;
        ArrayView<const dolfin::la_index> cdof_rowsQ, cdof_colsQ, cdof_rowsS, cdof_colsS;

        FormUtils::local_tensor_info(*(this->Q), *cell, &nrowsQ, cdof_rowsQ, &ncolsQ, cdof_colsQ);
        FormUtils::local_tensor_info(*(this->S), *cell, &nrowsS, cdof_rowsS, &ncolsS, cdof_colsS);
        Eigen::Matrix<double, Eigen::Dynamic, 1> Uglobal_e, Ulocal_e;
        Uglobal_e.resize(nrowsS);

        Uglobal.vector()->get_local(Uglobal_e.data(), nrowsS, cdof_rowsS.data());
        Ulocal_e  = invKS_list[cell->index()] * ( QRe_list[cell->index()] - LHe_list[cell->index()] * Uglobal_e );
        Ulocal.vector()->set_local(Ulocal_e.data(), nrowsQ, cdof_rowsQ.data());
    }
    Ulocal.vector()->apply("insert");
}
//-----------------------------------------------------------------------------
void PDEStaticCondensation::backsubtitute(const Function &Uglobal, Function &Ulocal, Function &Lambda){
    for( CellIterator cell(*(this->mesh)); !cell.end(); ++cell){
        // Backsubstitute global solution Uglobal to get local solution Ulocal as well as Lagrange
        // multiplier Lambda
        std::size_t nrowsQ, ncolsQ, nrowsR, ncolsR, nrowsS, ncolsS ;
        ArrayView<const dolfin::la_index> cdof_rowsQ, cdof_colsQ,
                                          cdof_rowsR, cdof_colsR,
                                          cdof_rowsS, cdof_colsS;

        FormUtils::local_tensor_info(*(this->Q), *cell, &nrowsQ, cdof_rowsQ, &ncolsQ, cdof_colsQ);
        FormUtils::local_tensor_info(*(this->R), *cell, &nrowsR, cdof_rowsR, &ncolsR, cdof_colsR);
        FormUtils::local_tensor_info(*(this->S), *cell, &nrowsS, cdof_rowsS, &ncolsS, cdof_colsS);
        Eigen::Matrix<double, Eigen::Dynamic, 1> Uglobal_e, Ulocal_e;
        Uglobal_e.resize(nrowsS);

        Uglobal.vector()->get_local(Uglobal_e.data(), nrowsS, cdof_rowsS.data());
        Ulocal_e  = invKS_list[cell->index()] * ( QRe_list[cell->index()] - LHe_list[cell->index()] * Uglobal_e );
        Ulocal.vector()->set_local(Ulocal_e.data(), nrowsQ, cdof_rowsQ.data());
        Lambda.vector()->set_local( (Ulocal_e.data()+nrowsQ), nrowsR, cdof_rowsR.data());
    }
    Ulocal.vector()->apply("insert");
    Lambda.vector()->apply("insert");
}
