set(sources_list exch-ind-disp30.cc 
                 exch12.cc 
                 sapt2.cc 
                 sapt.cc 
                 sapt0.cc 
                 usapt0.cc
                 exch-disp30.cc 
                 exch-disp20.cc 
                 amplitudes.cc 
                 utils.cc 
                 disp20.cc 
                 ind30.cc 
                 disp22sdq.cc 
                 ind-disp30.cc 
                 sapt2p.cc 
                 elst13.cc 
                 disp30.cc 
                 disp22t.cc 
                 ind20.cc 
                 elst10.cc 
                 exch-ind30.cc 
                 exch11.cc 
                 disp21.cc 
                 disp2ccd.cc 
                 exch10.cc 
                 exch-ind20.cc 
                 elst12.cc 
                 sapt2p3.cc 
                 ind22.cc 
)
psi4_add_module(lib sapt_solver sources_list mints)
