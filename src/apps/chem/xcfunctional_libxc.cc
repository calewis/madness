#include <madness/madness_config.h>
#include <chem/xcfunctional.h>
#include <madness/tensor/tensor.h>
#include <iostream>
#include <string>
#include <sstream>
#include <utility>
#include <madness/world/madness_exception.h>
#include <madness/world/MADworld.h>
#include <xc.h>
#include <xc_funcs.h>

// This function is here because compiling with -ffast-math breaks the C
// function isnan. Also, there is a bug in some compilers where isnan is
// undefined when <cmath> is included.
namespace {
    inline int isnan_x(double x) {
        volatile double y = x;
        return x != y;
    }
}

namespace madness {

static int lookup_name(const std::string& name) {
    // Call libxc routine
    return XC(functional_get_number(name.c_str()));
}

static std::string lookup_id(const int id) {
    // Call libxc routine, needs memory handling
    char *namep(XC(functional_get_name(id)));
    std::string name = (namep==NULL) ? "Functional not found" : std::string(namep);
    free(namep);
    return name;
}

static xc_func_type* make_func(int id, bool polarized) {
    xc_func_type* func = new xc_func_type;
    int POLARIZED = polarized ? XC_POLARIZED : XC_UNPOLARIZED;
    MADNESS_ASSERT(xc_func_init(func, id, POLARIZED) == 0);
    return func;
}

static xc_func_type* lookup_func(const std::string& name, bool polarized) {
    int id = lookup_name(name);
    MADNESS_ASSERT(id > 0);
    return make_func(id, polarized);
}

//XCfunctional::XCfunctional() {}
//XCfunctional::XCfunctional() : hf_coeff(0.0) {std::printf("Construct XC Functional from LIBXC Library");}
XCfunctional::XCfunctional() : hf_coeff(0.0) {
    rhotol=1e-7; rhomin=0.0;
    ggatol=1.e-4;
    nderiv=0;
    spin_polarized=false;
}

void XCfunctional::initialize(const std::string& input_line, bool polarized,
        World& world, const bool verbose) {
    rhotol=1e-7; rhomin=0.0; // default values
    ggatol=1.e-4;

    bool printit=verbose and (world.rank()==0);
    double factor;      // weight factor for the various functionals
    spin_polarized = polarized;


    std::stringstream line(input_line);
    std::string name;

    nderiv = 0;
    hf_coeff = 0.0;
    funcs.clear();

    if (printit) print("\nConstruct XC Functional from LIBXC Library");
    while (line >> name) {
        std::transform(name.begin(), name.end(), name.begin(), ::toupper);
        if (name == "LDA") {
            // Slater exchange and VWN-5 correlation
            funcs.push_back(std::make_pair(lookup_func("LDA_X",polarized),1.0));
            funcs.push_back(std::make_pair(lookup_func("LDA_C_VWN",polarized),1.0));
        } else if ((name == "BP86") or (name=="BP")) {
            // Becke exchange, VWN-3 correlation, Perdew correction
            funcs.push_back(std::make_pair(lookup_func("GGA_X_B88",polarized),1.0));
            funcs.push_back(std::make_pair(lookup_func("GGA_C_P86",polarized),1.0));
        } else if (name == "PBE") {
            funcs.push_back(std::make_pair(lookup_func("GGA_X_PBE",polarized),1.0));
            funcs.push_back(std::make_pair(lookup_func("GGA_C_PBE",polarized),1.0));
        } else if (name == "PBE0") {
            funcs.push_back(std::make_pair(lookup_func("GGA_X_PBE",polarized),0.75));
            funcs.push_back(std::make_pair(lookup_func("GGA_C_PBE",polarized),1.0));
            hf_coeff=0.25;
        } else if (name == "B3LYP") {
            // VWN-3 correlation
            funcs.push_back(std::make_pair(lookup_func("HYB_GGA_XC_B3LYP",polarized),1.0));
            hf_coeff=0.2;
        } else if (name == "RHOMIN") {
            line >> rhomin;
        } else if (name == "RHOTOL") {
            line >> rhotol;
        } else if (name == "GGATOL") {
            line >> ggatol;
        } else if (name == "HF" || name == "HF_X") {
            if (! (line >> factor)) factor = 1.0;
            hf_coeff = factor;
        } else {
            if (! (line >> factor)) factor = 1.0;
            funcs.push_back(std::make_pair(lookup_func(name,polarized), factor));
        }
    }

    for (unsigned int i=0; i<funcs.size(); i++) {
        if (funcs[i].first->info->family == XC_FAMILY_GGA) nderiv = std::max(nderiv,1);
        if (funcs[i].first->info->family == XC_FAMILY_HYB_GGA) nderiv = std::max(nderiv,1);
        if (funcs[i].first->info->family == XC_FAMILY_MGGA)nderiv = std::max(nderiv,2);
 //       if (funcs[i].first->info->family == XC_FAMILY_LDA) nderiv = std::max(nderiv,0);
    }
    if (printit) {
        print("\ninput line was:",input_line);
        for (std::size_t i=0; i<funcs.size(); ++i) {
            int id=funcs[i].first->info->number;
//            print(lookup_id(id),"with weight:",funcs[i].second);
            printf(" %4.3f %s \n",funcs[i].second,lookup_id(id).c_str());
        }
        if (hf_coeff>0.0) printf(" %4.3f %s \n",hf_coeff,"HF exchange");
        print("\nscreening parameters");
        print(" rhotol, rhomin",rhotol,rhomin);
        print("         ggatol",ggatol);
        if (printit) print("polarized ",polarized,"\n");

    }
}

XCfunctional::~XCfunctional() {
    for (unsigned int i=0; i<funcs.size(); i++) {
        xc_func_end(funcs[i].first);
        delete funcs[i].first;
    }
    funcs.clear();
}

bool XCfunctional::is_lda() const {
    return nderiv == 0;
}

bool XCfunctional::is_gga() const {
    return nderiv == 1;
}

bool XCfunctional::is_meta() const {
    return nderiv == 2;
}

bool XCfunctional::is_dft() const {
//    return (is_lda() || is_gga() || is_meta());
    return (funcs.size()>0);
}

bool XCfunctional::has_fxc() const
{
    return false; // not thought about this yet
}

bool XCfunctional::has_kxc() const
{
    return false;
}


void XCfunctional::make_libxc_args(const std::vector< madness::Tensor<double> >& xc_args,
           madness::Tensor<double>& rho, madness::Tensor<double>& sigma,
           madness::Tensor<double>& rho_pt, madness::Tensor<double>& sigma_pt,
           bool need_response) const {

    // number of grid points in this box
    const int np = xc_args[0].size();


    if (not spin_polarized) {
        if (is_lda()) {
            rho  = madness::Tensor<double>(np);
            const double * restrict rhoa = xc_args[enum_rhoa].ptr();
            double * restrict dens = rho.ptr();
            for (long i=0; i<np; i++) {
                dens[i] = munge(2.0*rhoa[i]);   // full dens is twice alpha dens
            }

            // add perturbed density in response calculations
            // note rho_pt does not depend on the spin
            if (need_response) {
                rho_pt  = madness::Tensor<double>(np);
                const double * restrict rho_pt1 = xc_args[enum_rho_pt].ptr();
                double * restrict dens_pt = rho_pt.ptr();
                for (long i=0; i<np; i++) {
                    dens_pt[i] = binary_munge(rho_pt1[i],rhoa[i]); // no factor 2
                }
            }
        }
        else if (is_gga()) {
            // rho is the density
            // the reduced density gradient sigma is given by
            // sigma = rho * rho * chi
            const double * restrict rhoa = xc_args[enum_rhoa].ptr();
            const double * restrict chiaa = xc_args[enum_chi_aa].ptr();
            rho  = madness::Tensor<double>(np);
            sigma  = madness::Tensor<double>(np);
            double * restrict dens = rho.ptr();
            double * restrict sig = sigma.ptr();
            for (long i=0; i<np; i++) {
                dens[i]=munge(2.0*rhoa[i]);     // full dens is twice alpha dens
                sig[i] = std::max(1.e-14,dens[i]*dens[i] * chiaa[i]);   // 2 factors 2 included in dens
            }

            // add perturbed density and density gradients in response calculations
            // note rho_pt does not depend on the spin
            if (need_response) {
                rho_pt  = madness::Tensor<double>(np);
                sigma_pt  = madness::Tensor<double>(np);

                const double * restrict rho_pt1 = xc_args[enum_rho_pt].ptr();
                const double * restrict sig_pt1 = xc_args[enum_sigma_pta_div_rho].ptr();

                double * restrict dens_pt = rho_pt.ptr();
                double * restrict sig_pt = sigma_pt.ptr();
                for (long i=0; i<np; i++) {
                    dens_pt[i] = binary_munge(rho_pt1[i],rhoa[i]);  // no factor 2
//                    sig_pt[i] = std::max(1.e-14,dens[i]*sig_pt1[i]);
                    sig_pt[i] = dens[i]*sig_pt1[i];
                    // dens is munged and includes factor of 2 for full density
                }
            }

        }
        else {
            MADNESS_EXCEPTION("only LDA and GGA available in xcfunctional",1);
        }

    } else if (spin_polarized) {
        if (is_lda()) {
            const double * restrict rhoa = xc_args[enum_rhoa].ptr();
            const double * restrict rhob = xc_args[enum_rhob].ptr();
            rho  = madness::Tensor<double>(np*2L);
            double * restrict dens = rho.ptr();

            // might happen if there are no beta electrons
            madness::Tensor<double> dummy;
            if (rhob==NULL) {
                dummy=madness::Tensor<double>(np);
                rhob=dummy.ptr();
            }

            for (long i=0; i<np; i++) {
                dens[2*i  ] = munge(rhoa[i]);
                dens[2*i+1] = munge(rhob[i]);
            }
            if (need_response) {
                MADNESS_EXCEPTION("no spin polarized DFT response in xcfunctional",1);
            }
        }
        else if (is_gga()) {
            const double * restrict rhoa  = xc_args[enum_rhoa].ptr();
            const double * restrict rhob  = xc_args[enum_rhob].ptr();

            const double * restrict chiaa = xc_args[enum_chi_aa].ptr();
            const double * restrict chiab = xc_args[enum_chi_ab].ptr();
            const double * restrict chibb = xc_args[enum_chi_bb].ptr();

            // might happen if there are no beta electrons
            madness::Tensor<double> dummy;
            if ((rhob==NULL) or (chiab==NULL) or (chibb==NULL)) {
                dummy=madness::Tensor<double>(np);
            }
            if (rhob==NULL) rhob=dummy.ptr();
            if (chiab==NULL) chiab=dummy.ptr();
            if (chibb==NULL) chibb=dummy.ptr();

            rho   = madness::Tensor<double>(np*2L);
            sigma = madness::Tensor<double>(np*3L);

            double * restrict dens = rho.ptr();
            double * restrict sig  = sigma.ptr();
            for (long i=0; i<np; i++) {

                double ra=munge(rhoa[i]);
                double rb=munge(rhob[i]);

                dens[2*i  ] = ra;
                dens[2*i+1] = rb;
                sig[3*i  ]  = std::max(1.e-14,ra * ra * chiaa[i]);  // aa
                sig[3*i+1]  = std::max(1.e-14,ra * rb * chiab[i]);  // ab
                sig[3*i+2]  = std::max(1.e-14,rb * rb * chibb[i]);  // bb

            }
            if (need_response) {
                MADNESS_EXCEPTION("no spin polarized DFT response in xcfunctional",1);
            }
        }
        else {
            MADNESS_EXCEPTION("only LDA and GGA available in xcfunctional",1);
        }
    }
}


madness::Tensor<double> XCfunctional::exc(const std::vector< madness::Tensor<double> >& t) const {
    madness::Tensor<double> rho, sigma, rho_pt, sigma_pt;
    make_libxc_args(t, rho, sigma, rho_pt, sigma_pt, false);

    const int np = t[0].size();
    const double * restrict dens = rho.ptr();
    const double * restrict sig = sigma.ptr();

    madness::Tensor<double> result(3L, t[0].dims());
    double * restrict res = result.ptr();
    for (long j=0; j<np; j++) res[j] = 0.0;

    for (unsigned int i=0; i<funcs.size(); i++) {
        madness::Tensor<double> zk(3L, t[0].dims(), false);
        double * restrict work = zk.ptr();

        switch(funcs[i].first->info->family) {
        case XC_FAMILY_LDA:
            xc_lda_exc(funcs[i].first, np, dens, work);
            break;
        case XC_FAMILY_GGA:
            xc_gga_exc(funcs[i].first, np, dens, sig, work);
            break;
        case XC_FAMILY_HYB_GGA:
            xc_gga_exc(funcs[i].first, np, dens, sig, work);
            break;
        default:
            throw "HOW DID WE GET HERE?";
        }
        if (spin_polarized) {
            for (long j=0; j<np; j++) {
                res[j] +=  work[j]*(dens[2*j+1] + dens[2*j])*funcs[i].second;
            }
        }
        else {
            for (long j=0; j<np; j++) {
                res[j] += work[j]*dens[j]*funcs[i].second;
            }
        }
    }
    return result;
}


madness::Tensor<double> XCfunctional::vxc(const std::vector< madness::Tensor<double> >& t,
        const int ispin, const xc_contribution xc_contrib) const {
    madness::Tensor<double> rho, sigma, dummy;
    make_libxc_args(t, rho, sigma, dummy, dummy, false);

    // number of grid points
    const int np = t[0].size();

    // number of intermediates depends on the spin
    int nvsig=1, nvrho=1;
    if (spin_polarized) {
        nvrho = 2;
        nvsig = 3;
    }

    madness::Tensor<double> result(3L, t[0].dims());
    double * restrict res = result.ptr();
    const double * restrict dens = rho.ptr();
    for (long j=0; j<np; j++) res[j] = 0.0;

    for (unsigned int i=0; i<funcs.size(); i++) {
        switch(funcs[i].first->info->family) {
        case XC_FAMILY_LDA:
        {
            madness::Tensor<double> vrho(nvrho*np);
            double * restrict vr = vrho.ptr();
            xc_lda_vxc(funcs[i].first, np, dens, vr);

            for (long j=0; j<np; j++) res[j] += vr[nvrho*j+ispin]*funcs[i].second;
        }

        break;

        case XC_FAMILY_HYB_GGA:
        case XC_FAMILY_GGA:
        {
            madness::Tensor<double> vrho(nvrho*np), vsig(nvsig*np);
            double * restrict vr = vrho.ptr();
            double * restrict vs = vsig.ptr();
            const double * restrict sig = sigma.ptr();
            // in: funcs[i].first
            // in: np      number of points
            // in: dens    the density [a,b]
            // in: sig     contracted density gradients \nabla \rho . \nabla \rho [aa,ab,bb]
            // out: vr     \del e/\del \rho_alpha [a,b]
            // out: vs     \del e/\del sigma_alpha [aa,ab,bb]
            xc_gga_vxc(funcs[i].first, np, dens, sig, vr, vs);

            if (spin_polarized) {
                if (xc_contrib == potential_rho) {
                    for (long j=0; j<np; j++) {                 // Vrhoa
                        res[j] += vr[nvrho*j+ispin] * funcs[i].second;
                    }
                }
                else if (xc_contrib == potential_same_spin) {   // Vsigaa/Vsigbb * rho
                    for (long j=0; j<np; j++) {
                        res[j] += vs[nvsig*j + 2*ispin] * funcs[i].second       // aa or bb in steps of 3
                                *dens[nvrho*j + ispin];                         // a or b in steps of 2
                    }
                }
                else if (xc_contrib == potential_mixed_spin) {  // Vsigab * rho_other_spin
                    for (long j=0; j<np; j++) {
                        res[j] += vs[nvsig*j + 1] * funcs[i].second             // ab in steps of 3
                                *dens[nvrho*j + (1-ispin)];                     // b or a in steps of 2
                    }
                }
                else {
                    throw "ouch";
                }
            }
            else {
                if (xc_contrib == potential_rho) {
                    for (long j=0; j<np; j++) {                 // Vrhoa
                        res[j] += vr[j]*funcs[i].second;
                    }
                }
                else if (xc_contrib == potential_same_spin) {   // Vsigaa
                    for (long j=0; j<np; j++) {
                        res[j] += vs[j]*funcs[i].second*dens[j];    // total density
                    }
                }
                else {
                    throw "ouch";
                }
            }
        }
        break;
        default:
            MADNESS_EXCEPTION("unknown XC_FAMILY xcfunctional::vxc",1);
        }
    }
    for (long j=0; j<np; j++) {
        if (isnan_x(res[j])) MADNESS_EXCEPTION("NaN in xcfunctional::vxc",1);
    }

    return result;
}


Tensor<double> XCfunctional::fxc_apply(const std::vector<Tensor<double> >& t,
        const int ispin, const xc_contribution xc_contrib) const {

    MADNESS_ASSERT(!spin_polarized);    // for now
    MADNESS_ASSERT(ispin==0);           // for now

    // copy quantities from t to rho and sigma
    Tensor<double> rho,sigma, rho_pt, sigma_pt;   // rho=2rho_alpha, sigma=4sigma_alpha
    make_libxc_args(t, rho, sigma, rho_pt, sigma_pt, true);

    // number of grid points
    const int np = t[0].size();

    // spin dimensions of the tensors
    const int nspin=(spin_polarized ? 2 : 1);   // rhf: 1; uhf: 2
    const int nspin2=nspin*(nspin+1)/2;         // rhf: 1; uhf: 3
    const int nspin3=nspin2*(nspin2+1)/2;       // rhf: 1; uhf: 6

    // intermediate tensors: partial derivatives of f_xc wrt rho/sigma
    Tensor<double> v2rho2(nspin2*np);       // lda, gga
    Tensor<double> v2rhosigma(nspin3*np);   // gga
    Tensor<double> v2sigma2(nspin3*np);     // gga
    Tensor<double> vrho(nspin*np);          // gga
    Tensor<double> vsigma(nspin2*np);       // gga

    // result tensor
    Tensor<double> result(3L, t[0].dims());

    for (unsigned int i=0; i<funcs.size(); i++) {
        switch(funcs[i].first->info->family) {
        case XC_FAMILY_LDA: {
            double * restrict vr = v2rho2.ptr();
            const double * restrict dens = rho.ptr();
            xc_lda_fxc(funcs[i].first, np, dens, vr);
        }
        break;

        case XC_FAMILY_HYB_GGA:
        case XC_FAMILY_GGA:
        {
            if ((xc_contrib == XCfunctional::kernel_second_semilocal) or
                    (xc_contrib== XCfunctional::kernel_second_local)) {   // partial second derivatives
                double * restrict vrr = v2rho2.ptr();
                double * restrict vrs = v2rhosigma.ptr();
                double * restrict vss = v2sigma2.ptr();
                const double * restrict sig = sigma.ptr();
                const double * restrict dens = rho.ptr();

                // in: funcs[i].first
                // in: np      number of points
                // in: dens    the density [a,b], or 2*\rho_alpha
                // in: sig     contracted density gradients \nabla \rho . \nabla \rho [aa,ab,bb]
                // out: vrr     \del^2 e/\del \rho^2_alpha [a,b]
                // out: vrs     \del^2 e/\del \sigma_alpha\rho [aa,ab,bb]
                // out: vss     \del^2 e/\del \sigma^2_alpha [aa,ab,bb]
                xc_gga_fxc(funcs[i].first, np, dens, sig, vrr, vrs, vss);

            } else if (xc_contrib == XCfunctional::kernel_first_semilocal) {   // partial first derivatives
                double * restrict vr = vrho.ptr();
                double * restrict vs = vsigma.ptr();
                const double * restrict sig = sigma.ptr();
                const double * restrict dens = rho.ptr();

                // in: funcs[i].first
                // in: np      number of points
                // in: dens    the density [a,b]
                // in: sig     contracted density gradients \nabla \rho . \nabla \rho [aa,ab,bb]
                // out: vr     \del e/\del \rho_alpha [a,b]
                // out: vs     \del e/\del sigma_alpha [aa,ab,bb]
                xc_gga_vxc(funcs[i].first, np, dens, sig, vr, vs);

            }
        }
        break;
        default:
            MADNESS_EXCEPTION("unknown XC_FAMILY xcfunctional::fxc",1);
        }

        Tensor<double> result1;
        // GGA, requires 3 terms
        // multiply the kernel with the various densities
        if (xc_contrib== XCfunctional::kernel_second_local) {  // local terms, second derivative
            result1=v2rho2.emul(rho_pt);
            if (is_gga()) result1+=2.0*v2rhosigma.emul(sigma_pt);

        } else if (xc_contrib== XCfunctional::kernel_second_semilocal) {   // semilocal terms, second derivative
            result1=2.0*v2rhosigma.emul(rho_pt) + 4.0*v2sigma2.emul(sigma_pt);

        } else if (xc_contrib== XCfunctional::kernel_first_semilocal) {   // semilocal terms, first derivative
            result1=2.0*vsigma;
        } else {
            MADNESS_EXCEPTION("confused xc_contrib in fxc_apply",1);
        }

        // accumulate into result tensor with proper weighting
        result+=result1*funcs[i].second;
    }

    // check for NaNs
    double * restrict res = result.ptr();
    for (long j=0; j<np; j++) if (isnan_x(res[j]))
        MADNESS_EXCEPTION("NaN in xcfunctional::fxc_apply",1);
    return result;
}

}
