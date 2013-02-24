#include <mpi.h>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <math.h>
#include "computelib.h"


using namespace std;

    conductance		cpl_cef;
    celltype1** 	smc;
    celltype2** 	ec;
    double		**sendbuf,**recvbuf;
    grid_parms		grid;

int 	CASE=1;
///***************************************************************************************/
///************ checked_malloc(size_t bytes, FILE* errfile, const char* errmsg)*************/
///***************************************************************************************/
void* checked_malloc(size_t bytes, FILE* errfile, const char* errmsg){
	void *pval = malloc(bytes);

	if (pval == NULL) {
		fprintf(errfile, "Allocation failed for %s\n", errmsg);
		MPI_Abort(MPI_COMM_WORLD, 100);
	}

	return pval;
}

int main(int argc, char* argv[]) {

//// These are input parameters for the simulation
	int m = 5,	///number of grid points in axial direction
			n = 5,	///number of grid points in circumferential direction
			e = 2,	///number of ECs per node
			s = 3;	///number of SMCs per node

	for (int i = 0; i < argc; i++) {
		if (argv[i][0] == '-') {
			if ((argv[i][1] == 'm')) {
				m = atoi(argv[i + 1]);
			} else if ((argv[i][1] == 'n')) {
				n = atoi(argv[i + 1]);
			}
		}
	}
	grid.m = m;
	grid.n = n;
///Time variables
	double tfinal = 100.00;
	double interval = 1e-2;
//File written every 1 second
	int file_write_per_unit_time = 1;	//int(1/interval);

	grid.uniform_jplc = 0.1, grid.min_jplc = 0.35, grid.max_jplc = 1.195, grid.gradient =
			2.5e-2;

///Global variables that are to be read by each processor
	int ndims, nbrs[4], dims[2], periodic[2], reorder = 0, coords[2];

///Global declaration of request and status update place holders.
///Request and Status handles for nonblocking Send and Receive operations, for communicating with each of the four neighbours.
	MPI_Request reqs[8];
	MPI_Status stats[8];

///Initialize MPI
	MPI_Init(&argc, &argv);

	grid.universe = MPI_COMM_WORLD;

	for (int i=0; i<4; i++){
		grid.nbrs[local][i]	= MPI_PROC_NULL;
		grid.nbrs[remote][i] = MPI_PROC_NULL;
	}
	int source, dest;
	int tag_local = 1,///tag for messaging information to local nearset neighbour
			tag_remote = 2;	///tag for messaging information to remote nearset neighbour

//Reveal information of myself and size of MPI_COMM_WORLD

	check_flag(MPI_Comm_rank(grid.universe, &grid.universal_rank), stdout,
			"error Comm_rank");
	check_flag(MPI_Comm_size(grid.universe, &grid.numtasks), stdout,
			"error Comm_size");

//Since there are 3 branches, there needs to be three values of a variable color, to identify association of a rank to a particular sub-universe partitioned out of MPI_COMM_WORLD.

	grid.color = int(grid.universal_rank / (grid.m * grid.n));
	grid.key = grid.color * ((grid.m * grid.n) - 1);

	check_flag(MPI_Comm_split(grid.universe, grid.color, grid.key, &grid.split_comm),
			stdout, "Comm-split failed");

	ndims = 2;
	dims[0] = grid.m;
	dims[1] = grid.n;
	periodic[0] = 0;
	periodic[1] = 1;
	reorder = 0;

	check_flag(
			MPI_Cart_create(grid.split_comm, ndims, dims, periodic, reorder,
					&grid.cart_comm), stdout, "failed at cart create");
	check_flag(MPI_Comm_rank(grid.cart_comm, &grid.rank), stdout,
			"failed at comm rank");
	check_flag(MPI_Cart_coords(grid.cart_comm, grid.rank, ndims, grid.coords),
			stdout, "failed at cart coords");

	check_flag(
			MPI_Cart_shift(grid.cart_comm, 0, 1, &grid.nbrs[local][UP],
					&grid.nbrs[local][DOWN]), stdout,
			"failed at cart shift up down");
	check_flag(
			MPI_Cart_shift(grid.cart_comm, 1, 1, &grid.nbrs[local][LEFT],
					&grid.nbrs[local][RIGHT]), stdout,
			"failed at cart left right");

///Initialize checkpoint routine which opens files
	checkpoint_handle *check = initialise_checkpoint(grid.rank);




///Each tasks now calculates the number of ECs per node.
	if (grid.m != (grid.numtasks / grid.n))
		e = grid.m / grid.numtasks;

///Each tasks now calculates the number of ECs per node.
	///topological information of a functional block of coupled cells. This is the minimum required to simulate a relevant coupled topology.
	grid.num_smc_fundblk_circumferentially = 1, grid.num_ec_fundblk_circumferentially =
			5, grid.num_smc_fundblk_axially = 13, grid.num_ec_fundblk_axially =
			1,

	grid.num_ghost_cells = 2,

	grid.num_fluxes_smc = 12;///number of SMC Ioinic currents to be evaluated for eval of LHS of the d/dt terms of the ODEs.
	grid.num_fluxes_ec = 12;///number of EC Ioinic currents to be evaluated for eval of LHS of the d/dt terms of the ODEs.

	grid.num_coupling_species_smc = 3;///number of SMC coupling species homogenic /heterogenic
	grid.num_coupling_species_ec = 3;///number of SMC coupling species homogenic /heterogenic

	grid.neq_smc = 5;			/// number of SMC ODEs for a single cell
	grid.neq_ec = 4;			/// number of EC ODEs for a single cell

	grid.num_ec_axially = e * 1;
	grid.num_smc_axially = e * 13;
	grid.num_ec_circumferentially = s * 5;
	grid.num_smc_circumferentially = s * 1;

	grid.neq_ec_axially = grid.num_ec_axially * grid.neq_ec;
	grid.neq_smc_axially = grid.num_smc_axially * grid.neq_smc;

	//Identifying remote neighbours

	grid.offset_P = 0;
	grid.offset_L = (grid.m * grid.n) + ((grid.m - 1) * grid.n);
	grid.offset_R = 2 * (grid.m * grid.n) + ((grid.m - 1) * grid.n);

	//check whether number of processors in circumferential direction are EVEN or ODD.
	grid.scheme = grid.n % 2;
	for(int i=0; i<4; i++){
		grid.flip_array[i]	=	0;
	}
	grid.branch_tag		=	0;

	//If number of processors in circumferentail dimension are EVEN
	if (grid.scheme == 0) {
		//For parent branch edge
		if ((grid.universal_rank >= 0) && (grid.universal_rank < grid.n)) {
			grid.branch_tag	=	P;
			if ((grid.universal_rank - grid.offset_P) < (grid.n / 2)) {
				grid.nbrs[remote][UP1] = grid.offset_L
						+ (grid.universal_rank - grid.offset_P);
				grid.nbrs[remote][UP2] = grid.offset_L
						+ (grid.universal_rank - grid.offset_P);
			} else if ((grid.universal_rank - grid.offset_P) >= (grid.n / 2)) {
				grid.nbrs[remote][UP1] = grid.offset_R
						+ (grid.universal_rank - grid.offset_P);
				grid.nbrs[remote][UP2] = grid.offset_R
						+ (grid.universal_rank - grid.offset_P);
			}
			//For Left daughter branch edge
		} else if ((grid.universal_rank >= grid.offset_L)
				&& (grid.universal_rank < (grid.offset_L + grid.n))) {
			grid.branch_tag	=	L;
			if ((grid.universal_rank - grid.offset_L) < (grid.n / 2)) {
				grid.nbrs[remote][DOWN1] = grid.universal_rank - grid.offset_L;
				grid.nbrs[remote][DOWN2] = grid.universal_rank - grid.offset_L;
			} else if ((grid.universal_rank - grid.offset_L) >= (grid.n / 2)) {
				grid.nbrs[remote][DOWN1] = (grid.offset_R + (grid.n - 1))
						- (grid.universal_rank - grid.offset_L);
				grid.nbrs[remote][DOWN2] = (grid.offset_R + (grid.n - 1))
						- (grid.universal_rank - grid.offset_L);
				grid.flip_array[DOWN1]	=	1;
				grid.flip_array[DOWN2]	=	1;
			}
		}
		//For Right daughter branch edge
		else if ((grid.universal_rank >= grid.offset_R)
				&& (grid.universal_rank < (grid.offset_R + grid.n))) {
			grid.branch_tag	=	R;
			if ((grid.universal_rank - grid.offset_R) < (grid.n / 2)) {
				grid.nbrs[remote][DOWN1] = (grid.offset_L + (grid.n - 1))
						- (grid.universal_rank - grid.offset_R);
				grid.nbrs[remote][DOWN2] = (grid.offset_L + (grid.n - 1))
						- (grid.universal_rank - grid.offset_R);
				grid.flip_array[DOWN1]	=	1;
				grid.flip_array[DOWN2]	=	1;
			} else if ((grid.universal_rank - grid.offset_R) >= (grid.n / 2)) {
				grid.nbrs[remote][DOWN1] = grid.universal_rank - grid.offset_R;
				grid.nbrs[remote][DOWN2] = grid.universal_rank - grid.offset_R;
			}
		}
	}

    //In the case of n being ODD

	if (grid.scheme != 0) {
		//The parent artery edge
		if ((grid.universal_rank >= 0) && (grid.universal_rank < grid.n)) {
			grid.branch_tag	=	P;
			if ((grid.universal_rank - grid.offset_P) < ((grid.n - 1) / 2)) {
				grid.nbrs[remote][UP1] = grid.offset_L + (grid.universal_rank - grid.offset_P);
				grid.nbrs[remote][UP2] = grid.offset_L + (grid.universal_rank - grid.offset_P);
			} else if ((grid.universal_rank - grid.offset_P) > ((grid.n - 1) / 2)) {
				grid.nbrs[remote][UP1] = grid.offset_R + (grid.universal_rank - grid.offset_P);
				grid.nbrs[remote][UP2] = grid.offset_R + (grid.universal_rank - grid.offset_P);
			} else if ((grid.universal_rank - grid.offset_P) == ((grid.n - 1) / 2)) {
				grid.nbrs[remote][UP1] = grid.offset_L + (grid.universal_rank - grid.offset_P);
				grid.nbrs[remote][UP2] = grid.offset_R + (grid.universal_rank - grid.offset_P);
			}
		}
		//The left daughter artery edge
		else if ((grid.universal_rank >= grid.offset_L) && (grid.universal_rank < grid.offset_L + grid.n)) {
			grid.branch_tag	=	L;
			if ((grid.universal_rank - grid.offset_L) < ((grid.n - 1) / 2)) {
				grid.nbrs[remote][DOWN1] = (grid.universal_rank - grid.offset_L);
				grid.nbrs[remote][DOWN2] = (grid.universal_rank - grid.offset_L);
			} else if ((grid.universal_rank - grid.offset_L) > ((grid.n - 1) / 2)) {
				grid.nbrs[remote][DOWN1] = (grid.offset_R + (grid.n-1)) - (grid.universal_rank - grid.offset_L);
				grid.nbrs[remote][DOWN2] = (grid.offset_R + (grid.n-1)) - (grid.universal_rank - grid.offset_L);
				grid.flip_array[DOWN1]	=	1;
				grid.flip_array[DOWN2]	=	1;
			} else if ((grid.universal_rank - grid.offset_L) == ((grid.n - 1) / 2)) {
				grid.nbrs[remote][DOWN1] = (grid.universal_rank - grid.offset_L);
				grid.nbrs[remote][DOWN2] = (grid.offset_R + (grid.n-1)) - (grid.universal_rank - grid.offset_L);
				grid.flip_array[DOWN1]	=	0;
				grid.flip_array[DOWN2]	=	1;
			}
		}
		//The right daughter artery edge
		else if ((grid.universal_rank >= grid.offset_R) && (grid.universal_rank < grid.offset_R + grid.n)) {
			grid.branch_tag	=	R;
			if ((grid.universal_rank - grid.offset_R) < ((grid.n - 1) / 2)) {
				grid.nbrs[remote][DOWN1] = (grid.offset_L + (grid.n-1)) - (grid.universal_rank - grid.offset_R);
				grid.nbrs[remote][DOWN2] = (grid.offset_L + (grid.n-1)) - (grid.universal_rank - grid.offset_R);
				grid.flip_array[DOWN1]	=	1;
				grid.flip_array[DOWN2]	=	1;
			} else if ((grid.universal_rank - grid.offset_R) > ((grid.n - 1) / 2)) {
				grid.nbrs[remote][DOWN1] = grid.universal_rank - grid.offset_R;
				grid.nbrs[remote][DOWN2] = grid.universal_rank - grid.offset_R;
			} else if ((grid.universal_rank - grid.offset_R) == ((grid.n - 1) / 2)) {
				grid.nbrs[remote][DOWN1] = (grid.offset_L + (grid.n-1)) - (grid.universal_rank - grid.offset_R);
				grid.nbrs[remote][DOWN2] = grid.universal_rank - grid.offset_R;
				grid.flip_array[DOWN1]	=	1;
				grid.flip_array[DOWN2]	=	0;
			}
		}
	}
///Now allocate memory space for the structures represegird.nting the cells and the various members of those structures.

//Each of the two cell grids have two additional rows and two additional columns as ghost cells.
//Follwing is an example of a 5x7 grid with added ghost cells on all four sides. the 0s are the actual
//members of the grid whereas the + are the ghost cells.

// + + + + + + + + +
// + 0 0 0 0 0 0 0 +
// + 0 0 0 0 0 0 0 +
// + 0 0 0 0 0 0 0 +
// + 0 0 0 0 0 0 0 +
// + 0 0 0 0 0 0 0 +
// + + + + + + + + +


	smc 	= (celltype1**) checked_malloc((grid.num_smc_circumferentially+grid.num_ghost_cells)* sizeof(celltype1*), stdout, "smc");
	for (int i=0; i<(grid.num_smc_circumferentially+grid.num_ghost_cells); i++){
		smc[i]	= (celltype1*) checked_malloc((grid.num_smc_axially+grid.num_ghost_cells)* sizeof(celltype1), stdout, "smc column dimension");

	}
	ec 	= (celltype2**) checked_malloc((grid.num_ec_circumferentially+grid.num_ghost_cells)* sizeof(celltype2*), stdout, "ec");
	for (int i=0; i<(grid.num_ec_circumferentially+grid.num_ghost_cells); i++){
		ec[i]	= (celltype2*) checked_malloc((grid.num_ec_axially+grid.num_ghost_cells)* sizeof(celltype2), stdout, "ec column dimension");
	}



///Memory allocation for state vector, the single cell evaluation placeholders (The RHS of the ODEs for each cell) and coupling fluxes is implemented in this section.
///In ghost cells, only the state vector array for each type of cells exists including all other cells.
///The memory is allocated for all the cells except the ghost cells, hence the ranges 1 to grid.num_ec_circumferentially(inclusive).
	///SMC domain
	for (int i = 0; i < (grid.num_smc_circumferentially+grid.num_ghost_cells); i++) {
		for (int j = 0; j < (grid.num_smc_axially+grid.num_ghost_cells); j++) {

			smc[i][j].p	=	(double*)checked_malloc(grid.neq_smc*sizeof(double),stdout,"allocation of array for state variables failed");
			smc[i][j].A = (double*) checked_malloc(
					grid.num_fluxes_smc * sizeof(double), stdout,
					"matrix A in smc");
			smc[i][j].B = (double*) checked_malloc(
					grid.num_coupling_species_smc * sizeof(double), stdout,
					"matrix B in smc");
			smc[i][j].C = (double*) checked_malloc(
					grid.num_coupling_species_smc * sizeof(double), stdout,
					"matrix C in smc");
		}
	}

	///EC domain
	for (int i = 0; i < (grid.num_ec_circumferentially+grid.num_ghost_cells); i++) {
		for (int j = 0; j < (grid.num_ec_axially+grid.num_ghost_cells); j++) {
			ec[i][j].q	=	(double*)checked_malloc(grid.neq_ec*sizeof(double),stdout,"allocation of array for state variables failed");

			ec[i][j].A = (double*) checked_malloc(
					grid.num_fluxes_ec * sizeof(double), stdout,
					"matrix A in ec");
			ec[i][j].B = (double*) checked_malloc(
					grid.num_coupling_species_ec * sizeof(double), stdout,
					"matrix B in ec");
			ec[i][j].C = (double*) checked_malloc(
					grid.num_coupling_species_ec * sizeof(double), stdout,
					"matrix C in ec");
		}
	}




	///Allocating memory space for coupling data to be sent and received by MPI communication routines.

	///sendbuf and recvbuf are 2D arrays having up,down,left and right directions as their first dimension.
	///Each dimension is broken down into two segments, e.g. up1,up2,down1 & down2,etc..
	///The length of the second dimension is equal to half the number of cells for which the information is to be sent and received.
	///Thus each communicating pair will exchange data twice to get the full lenght.

	sendbuf = (double**) checked_malloc(8 * sizeof(double*), stdout,
			"sendbuf dimension 1");
	recvbuf = (double**) checked_malloc(8 * sizeof(double*), stdout,
			"recvbuf dimension 1");

	///Each processor now allocates the memory for send and recv buffers those will hold the coupling information.
	///Since sendbuf must contain the information of number of SMCs and ECs being sent in the directions,
	///the first two elements contain the total count of SMCs located on the rank in the relevant dimension (circumferential or axial) and the count of SMCs for which
	///information is being sent, respectively.
	///The next two elements contain the same information for ECs.

	int extent_s, extent_e;	///Variables to calculate the length of the prospective buffer based on number of cell in either orientations (circumferential or axial).

	grid.added_info_in_send_buf = 4;///Number of elements containing additional information at the beginning of the send buffer.
	int seg_config_s, seg_config_e;	///Integers to decided whether the row or column being sent is overlapping or exactly divisible into two halfs.
	/// data to send to the neighbour in UP1 direction
	extent_s = (int) (ceil((double) (grid.num_smc_circumferentially) / 2));
	extent_e = (int) (ceil((double) (grid.num_ec_circumferentially) / 2));

	/// The seg_config variables are to recording the configuration of the split of the buffering in each direction.
	/// If the total number of cell in on a face (UP, DOWN, LEFT or RIGHT) are EVEN (i.e. seg_congif=0), the split will be non-overlapping
	/// (eg. if total SMCs are 26 in UP direction, UP1 buffer will send 13 and UP2 will send the other 13 to corresponding nbrs.
	/// If the total number of cells is ODD (i.e. seg_congif=1), then the split will be over lapping.
	/// (eg. if total SMCs are 13 (or any multiple of 13) UP1 will send elements from 0 - 6 and UP2 will send 6 - 12  to corresponding nbrs.
	/// These variables are used in send and recv buffers update before and after the MPI-communication routine is called.
	seg_config_s = grid.num_smc_circumferentially % 2;
	seg_config_e = grid.num_ec_circumferentially % 2;

	///Recording the number of elements in Send buffer in Up direction (UP1 or UP2) for use in update routine as count of elements.
	grid.num_elements_send_up = grid.added_info_in_send_buf
			+ (grid.num_coupling_species_smc * extent_s
					+ grid.num_coupling_species_ec * extent_e);

	sendbuf[UP1] = (double*) checked_malloc(
			grid.num_elements_send_up * sizeof(double), stdout,
			"sendbuf[UP1] dimension 2");

	sendbuf[UP1][0] = (double) (1); //Start of the 1st segment of SMC array in  in UP direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[UP1][1] = (double) (extent_s); //End of the 1st segment of SMC array in UP direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[UP1][2] = (double) (1); //Start of the 1st segment of EC array in UP direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[UP1][3] = (double) (extent_e); ///End of the 1st segment of EC array in UP direction (circumferential direction) to be sent to neighbouring processor

	sendbuf[UP2] = (double*) checked_malloc(
			grid.num_elements_send_up * sizeof(double), stdout,
			"sendbuf[UP2] dimension 2");

	if (seg_config_s != 0) {
		sendbuf[UP2][0] = (double) (extent_s); //Start of the 2nd segment of SMC array in UP direction (circumferential direction) to be sent to neighbouring processor
	} else if (seg_config_s == 0) {
		sendbuf[UP2][0] = (double) (extent_s + 1); //Start of the 2nd segment of SMC array in UP direction (circumferential direction) to be sent to neighbouring processor
	}
	sendbuf[UP2][1] = (double) (grid.num_smc_circumferentially); //End of the 2nd segment of SMC array in  in UP direction (circumferential direction) to be sent to neighbouring processor

	if (seg_config_e != 0) {
		sendbuf[UP2][2] = (double) (extent_e); //Start of the 2nd segment of EC array in  in UP direction (circumferential direction) to be sent to neighbouring processor
	} else if (seg_config_e == 0) {
		sendbuf[UP2][2] = (double) (extent_e + 1); //Start of the 2nd segment of EC array in  in UP direction (circumferential direction) to be sent to neighbouring processor
	}
	sendbuf[UP2][3] = (double) (grid.num_ec_circumferentially); //End of the 2nd segment of EC array in  in UP direction (circumferential direction) to be sent to neighbouring processor


	/// data to send to the neighbour in DOWN direction
	///Recording the number of elements in Send buffer in DOWN direction (DOWN1 or DOWN2) for use in update routine as count of elements.
	grid.num_elements_send_down = grid.added_info_in_send_buf
			+ (grid.num_coupling_species_smc * extent_s
					+ grid.num_coupling_species_ec * extent_e);

	sendbuf[DOWN1] = (double*) checked_malloc(
			grid.num_elements_send_down * sizeof(double), stdout,
			"sendbuf[DOWN1] dimension 2");

	sendbuf[DOWN1][0] = (double) (1); //Start of the 1st segment of SMC array in  in DOWN direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[DOWN1][1] = (double) (extent_s); //End of the 1st segment of SMC array in DOWN direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[DOWN1][2] = (double) (1); //Start of the 1st segment of EC array in DOWN direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[DOWN1][3] = (double) (extent_e); ///End of the 1st segment of EC array in DOWN direction (circumferential direction) to be sent to neighbouring processor

	sendbuf[DOWN2] = (double*) checked_malloc(
			grid.num_elements_send_down * sizeof(double), stdout,
			"sendbuf[DOWN2] dimension 2");
	if (seg_config_s != 0) {
		sendbuf[DOWN2][0] = (double) (extent_s); //Start of the 2nd segment of SMC array in DOWN direction (circumferential direction) to be sent to neighbouring processor
	} else if (seg_config_s == 0) {
		sendbuf[DOWN2][0] = (double) (extent_s + 1); //Start of the 2nd segment of SMC array in DOWN direction (circumferential direction) to be sent to neighbouring processor
	}

	sendbuf[DOWN2][1] = (double) (grid.num_smc_circumferentially); //End of the 2nd segment of SMC array in  in DOWN direction (circumferential direction) to be sent to neighbouring processor
	if (seg_config_e != 0) {
		sendbuf[DOWN2][2] = (double) (extent_e); //Start of the 2nd segment of EC array in  in DOWN direction (circumferential direction) to be sent to neighbouring processor
	} else if (seg_config_e == 0) {
		sendbuf[DOWN2][2] = (double) (extent_e + 1); //Start of the 2nd segment of EC array in  in DOWN direction (circumferential direction) to be sent to neighbouring processor
	}
	sendbuf[DOWN2][3] = (double) (grid.num_ec_circumferentially); //End of the 2nd segment of EC array in  in DOWN direction (circumferential direction) to be sent to neighbouring processor

	/// data to send to the neighbour in LEFT direction
	extent_s = (int) (ceil((double) (grid.num_smc_axially) / 2));
	extent_e = (int) (ceil((double) (grid.num_ec_axially) / 2));
	seg_config_s = grid.num_smc_axially % 2;
	seg_config_e = grid.num_ec_axially % 2;

	///Recording the number of elements in Send buffer in Left direction (LEFT1 or LEFT2) for use in update routine as count of elements.
	grid.num_elements_send_left = grid.added_info_in_send_buf
			+ (grid.num_coupling_species_smc * extent_s
					+ grid.num_coupling_species_ec * extent_e);
	sendbuf[LEFT1] = (double*) checked_malloc(
			grid.num_elements_send_left * sizeof(double), stdout,
			"sendbuf[LEFT1] dimension 2");
	sendbuf[LEFT1][0] = (double) (1); //Start of the 1st segment of SMC array in  in LEFT direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[LEFT1][1] = (double) (extent_s); //END of the 1st segment of SMC array in  in LEFT direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[LEFT1][2] = (double) (1); //Start of the 1st segment of EC array in  in LEFT direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[LEFT1][3] = (double) (extent_e); //END of the 1st segment of EC array in  in LEFT direction (circumferential direction) to be sent to neighbouring processor

	sendbuf[LEFT2] = (double*) checked_malloc(
			grid.num_elements_send_left * sizeof(double), stdout,
			"sendbuf[LEFT2] dimension 2");
	if (seg_config_s != 0) {
		sendbuf[LEFT2][0] = (double) (extent_s); //Start of the 2nd segment of SMC array in LEFT direction (circumferential direction) to be sent to neighbouring processor
	} else if (seg_config_s == 0) {
		sendbuf[LEFT2][0] = (double) (extent_s + 1); //Start of the 2nd segment of SMC array in LEFT direction (circumferential direction) to be sent to neighbouring processor
	}
	sendbuf[LEFT2][1] = (double) (grid.num_smc_axially); //END of the 2nd segment of SMC array in LEFT direction (circumferential direction) to be sent to neighbouring processor
	if (seg_config_e != 0) {
		sendbuf[LEFT2][2] = (double) (extent_e); //Start of the 2nd segment of EC array in LEFT direction (circumferential direction) to be sent to neighbouring processor
	} else if (seg_config_e == 0) {
		sendbuf[LEFT2][2] = (double) (extent_e + 1); //Start of the 2nd segment of EC array in LEFT direction (circumferential direction) to be sent to neighbouring processor
	}
	sendbuf[LEFT2][3] = (double) (grid.num_ec_axially); //END of the 2nd segment of EC array in LEFT direction (circumferential direction) to be sent to neighbouring processor

	/// data to send to the neighbour in RIGHT direction

	///Recording the number of elements in Send buffer in RIGHT direction (RIGHT1 or RIGHT2) for use in update routine as count of elements.
	grid.num_elements_send_right = grid.added_info_in_send_buf
			+ (grid.num_coupling_species_smc * extent_s
					+ grid.num_coupling_species_ec * extent_e);

	sendbuf[RIGHT1] = (double*) checked_malloc(
			grid.num_elements_send_right * sizeof(double), stdout,
			"sendbuf[RIGHT1] dimension 2");
	sendbuf[RIGHT1][0] = (double) (1); //Start of the 1st segment of SMC array in  in RIGHT direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[RIGHT1][1] = (double) (extent_s); //END of the 1st segment of SMC array in  in RIGHT direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[RIGHT1][2] = (double) (1); //Start of the 1st segment of EC array in  in RIGHT direction (circumferential direction) to be sent to neighbouring processor
	sendbuf[RIGHT1][3] = (double) (extent_e); //END of the 1st segment of EC array in  in RIGHT direction (circumferential direction) to be sent to neighbouring processor

	sendbuf[RIGHT2] = (double*) checked_malloc(
			grid.num_elements_send_right * sizeof(double), stdout,
			"sendbuf[RIGHT2] dimension 2");
	if (seg_config_s != 0) {
		sendbuf[RIGHT2][0] = (double) (extent_s); //Start of the 2nd segment of SMC array in RIGHT direction (circumferential direction) to be sent to neighbouring processor
	} else if (seg_config_s == 0) {
		sendbuf[RIGHT2][0] = (double) (extent_s + 1); //Start of the 2nd segment of SMC array in RIGHT direction (circumferential direction) to be sent to neighbouring processor
	}
	sendbuf[RIGHT2][1] = (double) (grid.num_smc_axially); //END of the 2nd segment of SMC array in RIGHT direction (circumferential direction) to be sent to neighbouring processor
	if (seg_config_e != 0) {
		sendbuf[RIGHT2][2] = (double) (extent_e); //Start of the 2nd segment of EC array in RIGHT direction (circumferential direction) to be sent to neighbouring processor
	} else if (seg_config_e == 0) {
		sendbuf[RIGHT2][2] = (double) (extent_e + 1); //Start of the 2nd segment of EC array in RIGHT direction (circumferential direction) to be sent to neighbouring processor
	}
	sendbuf[RIGHT2][3] = (double) (grid.num_ec_axially); //END of the 2nd segment of EC array in RIGHT direction (circumferential direction) to be sent to neighbouring processor


	///Call communication to the number of elements to be recieved by neighbours and allocate memory of recvbuf for each direction accordingly.
		communicate_num_recv_elements_to_nbrs(stdout);
		///memory allocation

	/// data to receive from the neighbour in UP direction
	recvbuf[UP1] = (double*) checked_malloc(
			grid.num_elements_recv_up * sizeof(double), stdout,
			"recvbuf[UP1] dimension 2");
	recvbuf[UP2] = (double*) checked_malloc(
			grid.num_elements_recv_up * sizeof(double), stdout,
			"recvbuf[UP2] dimension 2");

	/// data to recv from the neighbour in DOWN direction
	recvbuf[DOWN1] = (double*) checked_malloc(
			grid.num_elements_recv_down * sizeof(double), stdout,
			"recvbuf[DOWN1] dimension 2");
	recvbuf[DOWN2] = (double*) checked_malloc(
			grid.num_elements_recv_down * sizeof(double), stdout,
			"recvbuf[DOWN2] dimension 2");

	/// data to receive from the neighbour in LEFT direction
	recvbuf[LEFT1] = (double*) checked_malloc(
			grid.num_elements_recv_left * sizeof(double), stdout,
			"recvbuf[LEFT1] dimension 2");
	recvbuf[LEFT2] = (double*) checked_malloc(
			grid.num_elements_recv_left * sizeof(double), stdout,
			"recvbuf[LEFT2] dimension 2");

	/// data to receive from the neighbour in RIGHT direction
	recvbuf[RIGHT1] = (double*) checked_malloc(
			grid.num_elements_recv_right * sizeof(double), stdout,
			"recvbuf[RIGHT1] dimension 2");
	recvbuf[RIGHT2] = (double*) checked_malloc(
			grid.num_elements_recv_right * sizeof(double), stdout,
			"recvbuf[RIGHT2] dimension 2");

	grid.NEQ =	grid.neq_smc*(grid.num_smc_axially*grid.num_smc_circumferentially) + grid.neq_ec*(grid.num_ec_axially*grid.num_ec_circumferentially);

	///Setup output streams to write data in files. Each node opens an independent set of files and write various state variables into it.
//	checkpoint_handle *check = initialise_checkpoint(myRank);


	///Setting up the solver

	double 	tnow	= 0.0;

	//Error control variables
	double 	TOL	= 1e-6;

	double * thres = (double*)checked_malloc(grid.NEQ*sizeof(double),stdout,"Threshod array for RKSUITE");

	for (int i=0; i<grid.NEQ; i++)
		thres[i]	=	1e-6;

	//Variables holding new and old values
	double* y =  (double*)checked_malloc(grid.NEQ*sizeof(double),stdout,"Solver array y for RKSUITE");
	double* yp = (double*) checked_malloc(grid.NEQ * sizeof(double), stdout,"Solver array yp for RKSUITE");



	///Initialize different state variables and coupling data values.
		Initialize_koeingsberger_smc(grid,y,smc);
		Initialize_koeingsberger_ec(grid,y,ec);


		map_solver_to_cells(grid,y,smc,ec);


int state 	=  couplingParms(CASE,&cpl_cef);
//dump_rank_info(check,cpl_cef,grid);

double t1	=	MPI_Wtime();

	rksuite_solver_CT(tnow, tfinal, interval, y, yp, grid.NEQ , TOL, thres, file_write_per_unit_time, check);

	//rksuite_solver_UT(tnow, tfinal, interval, y, yp, grid.NEQ,TOL,thres, file_write_per_unit_time,check);

double t2	=	MPI_Wtime();
	final_checkpoint(check, grid,t1, t2);
MPI_Finalize();
}// end main()























