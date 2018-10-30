#include "adddelete.h"

using namespace dolfin;

AddDelete::AddDelete(std::vector<std::shared_ptr<const Function>> flist)
{
    std::cout<<"Received"<<std::endl;
    // Test if you can do anything useful

    for (std::size_t i = 0; i < flist.size(); ++i)
    {
        std::cout<<"Space dim "<<flist[i]->function_space()->element()->space_dimension()
                   <<std::endl;
    }
}
//
AddDelete::AddDelete(particles &P, std::size_t np_min, std::size_t np_max,
                     std::vector<std::shared_ptr<const Function> > FList)
    : _P(&P), _np_min(np_min), _np_max(np_max), _FList(FList)
{
    // TODO: tests on input
}
//
AddDelete::AddDelete(particles &P, std::size_t np_min, std::size_t np_max,
                     std::vector<std::shared_ptr<const Function> > FList,
                     std::vector<std::size_t> pbound, std::vector<double> bounds)
    : AddDelete::AddDelete( P, np_min, np_max, FList )
{
    // TODO: tests on input
    // TODO: initalizatin of inserted values via bound
    // TODO: test that pbounds are only defined for scalar quantities, check function space!
    _pbound = pbound;
    _bounds = bounds;
}
//
AddDelete::~AddDelete(){}
//
void AddDelete::do_sweep(){
    for (CellIterator cell(*(_P->_mesh)); !cell.end(); ++cell){
        // Get number of particles
        std::size_t Npc = _P->_cell2part[cell->index()].size();

        if(Npc >= _np_min && Npc <= _np_max) continue;
        if (Npc < _np_min){
            std::size_t np_def = _np_min - Npc;
            insert_particles(np_def, *cell);
        }else if(Npc > _np_max)
        {
            std::size_t np_surp = Npc - _np_max;
            delete_particles(np_surp, Npc, cell->index());
        }
    }
}
//
// TODO: to be deprecated?
void AddDelete::do_sweep_weighted(){
    // This rather crude approach allows to do the sweep after the advection
    // particle properties for the freshly constructed particle are derived
    // from the neighboring particles

    // Iterate over cells
    for (CellIterator cell(*(_P->_mesh)); !cell.end(); ++cell){
        // Get number of particles
        std::size_t Npc = _P->_cell2part[cell->index()].size();

        if(Npc >= _np_min && Npc <= _np_max) continue;
        if (Npc < _np_min){
            std::size_t np_def = _np_min - Npc;
            insert_particles_weighted(np_def, *cell);
        }else if(Npc > _np_max)
        {
            std::size_t np_surp = Npc - _np_max;
            delete_particles(np_surp, Npc, cell->index());
        }
    }
}
//
void AddDelete::do_sweep_failsafe(const std::size_t np_min){
    // Method to do a failsafe sweep after advection to make sure
    // that cell contains at least minimum required amount of particles
    // Note that this failsafe will compromise on convergence, but it's
    // better than breaking

    // Iterate over cells
    for (CellIterator cell(*(_P->_mesh)); !cell.end(); ++cell){
        // Get number of particles
        std::size_t Npc = _P->_cell2part[cell->index()].size();

        if(Npc >= np_min) continue;
        if (Npc < np_min){
            std::cout<<"Failsafe activated in cell "<<cell->index()<<std::endl;
            std::size_t np_def = np_min - Npc;
            insert_particles_weighted(np_def, *cell);
        }
    }
}
//
void AddDelete::insert_particles(const std::size_t Np_def, const Cell& dolfin_cell){
    // Get vertex coords, returned as [x1, y1, z1, x2, y2, z2, ..., xn, yn, zn]
    std::vector<double> x_min_max, vertex_coordinates;
    dolfin_cell.get_vertex_coordinates(vertex_coordinates);

    // Get cell bounding box
    Utils::cell_bounding_box(x_min_max, vertex_coordinates, _P->_Ndim);

    // Needed for point generation and placement
    seed(Np_def * dolfin_cell.index() );
    ufc::cell ufc_cell;
    dolfin_cell.get_cell_data(ufc_cell);

    for(std::size_t pgen = 0; pgen < Np_def; pgen++){
        // Initialize random positions
        Point xp_new;
        initialize_random_position(xp_new, x_min_max, dolfin_cell);
        Array<double> xp_array(_P->_Ndim, xp_new.coordinates());

        particle pnew;
        pnew.push_back(xp_new);

        // Eval other properties and push
        for(std::size_t idx_func = 0; idx_func < _FList.size(); idx_func++ ){
            Array<double> feval(_P->_ptemplate[idx_func + 1 ]); // +1 to skip position slot
            _FList[idx_func]->eval(feval, xp_array, dolfin_cell, ufc_cell);

            // Convert to Point
            Point pproperty(_P->_ptemplate[idx_func + 1 ], feval.data());

            // Check if bounded update
            check_bounded_update(pproperty, idx_func);

            pnew.push_back(pproperty);
        }

        // If necessary, push back remaining slots (initialized with positions,
        // this in fact is even needed to support the multi-stage rk scheme
        while(pnew.size() < _P->_ptemplate.size() )
            pnew.push_back(xp_new);

        // Push back to particles
        _P->_cell2part[dolfin_cell.index()].push_back(pnew);
    }
}
//
void AddDelete::insert_particles_weighted(const std::size_t Np_def, const Cell& dolfin_cell){
    //
    std::size_t cidx = dolfin_cell.index();

    // Needed for particle position initiation
    std::vector<double> x_min_max, vertex_coordinates;
    dolfin_cell.get_vertex_coordinates(vertex_coordinates);

    // Get cell bounding box
    Utils::cell_bounding_box(x_min_max, vertex_coordinates, _P->_Ndim);
    ufc::cell ufc_cell;
    dolfin_cell.get_cell_data(ufc_cell);

    // Needed for point generation and placement
    seed(Np_def * cidx );

    for(std::size_t pgen = 0; pgen < Np_def; pgen++){
        // Initialize random positions
        Point xp_new;
        initialize_random_position(xp_new, x_min_max, dolfin_cell);

        particle pnew;
        pnew.push_back(xp_new);

        // Loop over other particles to compute weights
        std::vector<double> distance;
        double distance_sum(0);
        double distance_p;
        for(auto p: _P->_cell2part[cidx]){
            distance_p = xp_new.squared_distance(p[0]);
            distance.push_back(distance_p);
            distance_sum += distance_p;
        }

        if(distance_sum > 0.){
            // Then loop over the particle template
            for(std::size_t idx_func = 0; idx_func < _FList.size(); idx_func++ ){
                Point point_value;


                // Again the idx_func+1 for skipping position
                for( std::size_t pidx = 0; pidx < _P->_cell2part[cidx].size(); pidx++ )
                    point_value += distance[pidx]*_P->_cell2part[cidx][pidx][idx_func +1];

                point_value /= distance_sum;

                // Check if bounded update
                check_bounded_update(point_value, idx_func);

                pnew.push_back(point_value);
            }
        }else{
            std::cout<<"Cell "<<cidx
                <<" does not contain any particles, an interpolate from the old state is the best I can do"
                <<std::endl;

            // Eval other properties and push
            for(std::size_t idx_func = 0; idx_func < _FList.size(); idx_func++ ){
                Array<double> xp_array(_P->_Ndim, xp_new.coordinates());
                Array<double> feval(_P->_ptemplate[idx_func + 1 ]); // +1 to skip position slot
                _FList[idx_func]->eval(feval, xp_array, dolfin_cell, ufc_cell);

                // Convert to Point
                Point pproperty(_P->_ptemplate[idx_func + 1 ], feval.data());

                // Check if bounded update
                check_bounded_update(pproperty, idx_func);

                pnew.push_back(pproperty);
            }
        }

        // If necessary, push back remaining slots (initialized with positions,
        // this in fact is even needed to support the multi-stage rk scheme
        while(pnew.size() < _P->_ptemplate.size() )
            pnew.push_back(xp_new);

        _P->_cell2part[dolfin_cell.index()].push_back(pnew);
    }
}
//
void AddDelete::delete_particles(const std::size_t Np_surp, const std::size_t Npc, const std::size_t cidx){
    // Particles with minimum distances are removed
    double distance;

    std::vector<double> pdistance(2);
    std::vector<std::pair<double, std::size_t>> pdistance_pair;

    for(std::size_t pidx1=0; pidx1<Npc; pidx1++){
        Point xp1 = _P->_cell2part[cidx][pidx1][0];
        std::fill(pdistance.begin(), pdistance.end(), 999.);
        for(std::size_t pidx2=0; pidx2<Npc; pidx2++){
            if(pidx2 != pidx1){
                // Compute squared distance to point 2
                distance = xp1.squared_distance(_P->_cell2part[cidx][pidx2][0]);
                // Copy values
                if(distance < pdistance[0]){
                    pdistance[1] = pdistance[0];
                    pdistance[0] = distance;
                }
            }
        }
        // Store in pair
        pdistance_pair.push_back( std::make_pair(pdistance[0] + pdistance[1], pidx1) );
    }

    // Sort (ascending!)
    std::sort(pdistance_pair.begin(),pdistance_pair.end());
    std::vector<std::size_t> remove_idcs;

    for(std::size_t prmv = 0; prmv < Np_surp; prmv++){
        remove_idcs.push_back(pdistance_pair[prmv].second);
    }
    std::sort(remove_idcs.begin(), remove_idcs.end());

    // And finally remove
    for(std::size_t prmv = 0; prmv < Np_surp; prmv++){
        _P->_cell2part[cidx].erase(_P->_cell2part[cidx].begin() + (remove_idcs[prmv]- prmv) );
    }
}

void AddDelete::initialize_random_position(Point& xp_new, const std::vector<double>& x_min_max, const Cell& dolfin_cell){
    bool hit = false;
    std::size_t iter(0);
    std::size_t gdim = _P->_Ndim;

    while(!hit)
    {
        std::vector<double> xp_vec;
        for(std::size_t i = 0; i < gdim; i ++)
        {
            double x = x_min_max[i] + (x_min_max[gdim + i ] - x_min_max[i] ) * rand();
            xp_vec.push_back(x);
        }
        Point xp_dummy(gdim, xp_vec.data() );
        if(dolfin_cell.contains(xp_dummy) ){
            xp_new = xp_dummy;
            hit = true;
        }

        // Check if acceptable number of iterations
        iter++;
        if (iter > 100)
            dolfin_error("AddDelete.cpp::initialize_positions", "initialize_positions", "Taking way too many iterations");
    }
}

void AddDelete::check_bounded_update(Point& pfeval, const std::size_t idx_func){
    // Bounded update (convenient for discontinuous field)
    auto it = std::find(_pbound.begin(), _pbound.end(), idx_func + 1 );
    if(it != _pbound.end()){
        std::size_t idx = std::distance(_pbound.begin(), it);
        if( pfeval[0] > 0.5*(_bounds[2*idx] + _bounds[2*idx+1]) )
            // Then upper value
            pfeval[0] = _bounds[2*idx+1];
        else
            // Then lower value
            pfeval[0] = _bounds[2*idx];
    }//else do nothing
}