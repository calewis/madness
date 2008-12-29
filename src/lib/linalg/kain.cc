#include <iostream>
#include <tensor/tensor.h>
#include <linalg/tensor_lapack.h>
#include <world/print.h>

using namespace madness;
using namespace std;



/// Solves the KAIN equations for coefficients to compute the next vector

/// \verbatim
///   Q(i,j) = <xi|fj>
///   A(i,j) = <xi-xm | fj-fm> = Q(i,j) - Q(m,j) - Q(i,m) + Q(m,m)
///   b(i,j) =-<xi-xm | fm> = -Q(i,m) + Q(m,m)
///   A c = b
///
///   . Correction to vector m
///   .   interior = sum(i<m)[ c(i)*(x(i)-x(m)) ] = sum(i<m)[c(i)*x(i)] - x[m]*sum(i<m)[c(i)]
///   .   exterior = -f(m) - sum(i<m)[ c(i)*(f(i)-f(m)) ] = -f(m) - sum(i<m)[c(i)*f(i)] + f(m)*sum(i<m)[c(i)]
///   . New vector
///   .   define C = sum(i<m)(c(i))  (note less than)
///   .   define c(m) = 1.0 - C
///   .   xnew = sum(i<=m) [ c(i)*(x(i) - f(i)) ]
/// \endverbatim
template <typename T>
Tensor<T> KAIN(const Tensor<T>& Q) {
    const int nvec = Q.dim[0];
    const int m = nvec-1;

    if (nvec == 1) {
        Tensor<T> c(1);
        c(0L) = 1.0;
        return c;
    }

    Tensor<T> A(m,m);
    Tensor<T> b(m);
    for (long i=0; i<m; i++) {
        b(i) = Q(m,m) - Q(i,m);
        for (long j=0; j<m; j++) {
            A(i,j) = Q(i,j) - Q(m,j) - Q(i,m) + Q(m,m);
        }
    }

//     print("Q");
//     print(Q);
//     print("A");
//     print(A);
//     print("b");
//     print(b);

    double rcond = 1e-12;
    Tensor<T> x;
    Tensor<double> s;
    long rank;
    gelss(A, b, rcond, &x, &s, &rank);
    print("singular values", s);
    print("rank", rank);
    print("solution", x);

    Tensor<T> c(nvec);
    T sumC = 0.0;
    for (long i=0; i<m; i++) sumC += x(i);
    c(Slice(0,m-1)) = x;
    print("SUMC", nvec, m, sumC);
    c(m) = 1.0 - sumC;

    print("returned C", c);

    return c;
}


struct SolverTargetInterface {
    virtual bool provides_jacobian() const = 0;
    virtual Tensor<double> residual(const Tensor<double>& x) = 0;
    virtual Tensor<double> jacobian(const Tensor<double>& x) {
        throw "not implemented";
    }
    virtual void residual_and_jacobian(const Tensor<double>& x, 
                                       Tensor<double>& residual, Tensor<double>& jacobian) {
        residual = this->residual(x);
        jacobian = this->jacobian(x);
    }
    virtual ~SolverTargetInterface() {};
};


struct OptimizationTargetInterface {
    virtual bool provides_gradient() const = 0;

    virtual double value(const Tensor<double>& x) = 0;

    virtual Tensor<double> gradient(const Tensor<double>& x) {
        throw "not implemented";
    }

    virtual void value_and_gradient(const Tensor<double>& x,
                                    double& value, 
                                    Tensor<double>& gradient) {
        value = this->value(x);
        gradient = this->gradient(x);
    }
};


struct SolverInterface {
    virtual bool solve(Tensor<double>& x) = 0;
    virtual bool converged() const = 0;
    virtual double residual_norm() const = 0;
};

struct OptimizerInterface {
    virtual bool optimize(Tensor<double>& x) = 0;
    virtual bool converged() const = 0;
    virtual double value() const = 0;
    virtual double gradient_norm() const = 0;
};


class SteepestDescent : public OptimizerInterface {
    SharedPtr<OptimizationTargetInterface> target;
    const double tol;
    const double value_precision;  // Numerical precision of value
    const double gradient_precision; // Numerical precision of each element of residual
    double f;
    double gnorm;

public:
    SteepestDescent(const SharedPtr<OptimizationTargetInterface>& target,
                    double tol = 1e-6,
                    double value_precision = 1e-12,
                    double gradient_precision = 1e-12)
        : target(target)
        , tol(tol)
        , value_precision(value_precision)
        , gradient_precision(gradient_precision)
        , gnorm(tol*1e16)
    {
        if (!target->provides_gradient()) throw "Steepest descent requires the gradient";
    }

    bool optimize(Tensor<double>& x) {
        double step = 10.0;
        double  fnew;
        Tensor<double> g;
        target->value_and_gradient(x,f,g);
        gnorm = g.normf();
        for (int i=0; i<100; i++) {
            while (1) {
                Tensor<double> gnew;
                x.gaxpy(1.0, g, -step);
                target->value_and_gradient(x,fnew,gnew);
                if (fnew < f) {
                    f = fnew;
                    g = gnew;
                    break;
                }
                x.gaxpy(1.0, g, step);
                step *= 0.5;
                print("reducing step size", f, fnew, step);
            }
            Tensor<double> g = target->gradient(x);
            gnorm = g.normf();
            print("iteration",i,"value",f,"gradient",gnorm);
            if (converged()) break;
        }
        return converged();
    }

    bool converged() const {return gnorm < tol;}

    double gradient_norm() const {return gnorm;}

    double value() const {return f;}
};

class BFGS : public OptimizerInterface {
private:
    SharedPtr<OptimizationTargetInterface> target;
    const double tol;
    const double value_precision;  // Numerical precision of value
    const double gradient_precision; // Numerical precision of each element of residual
    double f;
    double gnorm;
    Tensor<double> h;
    int n;


    double line_search(double a1, double f0, double dxgrad, const Tensor<double>& x, const Tensor<double>& dx) {
        double f1, f2p;
        double hess, a2;
        const char* lsmode = "";
        
        if (dxgrad*a1 > 0.0) {
            print(" Warning ... line search gradient +ve ", a1, dxgrad);
            a1 = -a1;
        }
        
        f1 = target->value(x + a1 * dx);
        
        // Fit to a parabola using f0, g0, f1
        hess = 2.0*(f1-f0-a1*dxgrad)/(a1*a1);
        a2 = -dxgrad/hess;
        
        if (abs(f1-f0) < value_precision) { // Insufficient precision
            a2 = a1;
            lsmode = "fixed";
        }
        else if (hess > 0.0) { // Positive curvature
            if ((f1 - f0) <= -value_precision) { // Downhill
                lsmode = "downhill";
                if (abs(a2) > 4.0*abs(a1)) {
                    lsmode = "restrict";
                    a2 = 4.0*a1;
                }
            }
            else { // Uphill
                lsmode = "bracket";
            }
        }
        else { // Negative curvature
            if ((f1 - f0) < value_precision) { // Downhill
                lsmode = "negative";
                a2 = 2e0*a1;
            }
            else {
                lsmode = "punt";
                a2 = a1;
            }
        }
    
        f2p = f0 + dxgrad*a2 + 0.5*hess*a2*a2;
        printf("step=%.3f value=%.6f grad=%.2e hess=%.2e mode=%s newstep=%.3f predicted=%.6f\n",a1, f1, dxgrad, hess, lsmode, a2, f2p);

        return a2;
    }



    
    void hessian_update_bfgs(const Tensor<double>& dx, 
                             const Tensor<double>& g, 
                             const Tensor<double>& gp)
    {
        /*
          Apply the BFGS update to the approximate Hessian h[][]. 
          
          h[][] = Hessian matrix from previous iteration 
          dx[]  = Step from previous iteration 
          .       (dx[] = x[] - xp[] where xp[] is the previous point) 
          g[]   = gradient at current point 
          gp[]  = gradient at previous point 
          
          Returns the updated hessian 
        */
        
        Tensor<double> hdx  = inner(h,dx);
        Tensor<double> dg = g - gp;
        
        double dxhdx = dx.trace(hdx);
        double dxdx  = dx.trace(dx);
        double dxdg  = dx.trace(dg);
        double dgdg  = dg.trace(dg);
        
        if ( (dxdx > 0.0) && (dgdg > 0.0) && (abs(dxdg/sqrt(dxdx*dgdg)) > 1.e-8) ) {
            for (int i=0; i<n; i++) {
                for (int j=0; j<n; j++) {
                    h(i,j) += dg[i]*dg[j]/dxdg - hdx[i]*hdx[j]/dxhdx;
                }
            }
        }
        else {
            printf(" BFGS not updating dxdg (%e), dgdg (%e), dxhdx (%f), dxdx(%e)\n" , dxdg, dgdg, dxhdx, dxdx);
        }
    }

    Tensor<double> new_search_direction(const Tensor<double>& g) {
        Tensor<double> dx, s;
        double tol = gradient_precision;
        double trust = 1.0; // This applied in spectral basis

        Tensor<double> v, e;
        syev(h, &v, &e);

        // Transform gradient into spectral basis
        Tensor<double> gv = inner(g,v);

        // Take step applying restriction
        for (int i=0; i<n; i++) {
            if (e[i] < -tol) 
                e[i] = -2.0*e[i]; // Enforce positive search direction
            else if (e[i] < -tol) 
                e[i] = tol;
           
            gv[i] = -gv[i] / e[i];
            if (abs(gv[i]) > trust) // Step restriction
                gv[i] = trust/gv[i];
        }
        
        // Transform back from spectral basis
        return inner(v,gv);
    }

public:
    BFGS(const SharedPtr<OptimizationTargetInterface>& target,
         double tol = 1e-6,
         double value_precision = 1e-12,
         double gradient_precision = 1e-12)
        : target(target)
        , tol(tol)
        , value_precision(value_precision)
        , gradient_precision(gradient_precision)
        , gnorm(tol*1e16)
        , n(0)
    {
        if (!target->provides_gradient()) throw "BFGS requires the gradient";
    }
    
    bool optimize(Tensor<double>& x) {
        if (n != x.dim[0]) {
            n = x.dim[0];
            h = Tensor<double>(n,n);
            for (int i=0; i<n; i++) h(i,i) = 1.0;
        }

        Tensor<double> tt = target->gradient(x);
        for (int i=0; i<n; i++) {
            x[i] += 0.01;
            double fp = target->value(x);
            x[i] -= 0.02;
            double fm = target->value(x);
            x[i] += 0.01;
            double gtest = 0.5*(fp-fm)/0.01;
            print("gtest", i, gtest, tt[i]);
        }


        Tensor<double> gp, dx;
        double fp;
        for (int iter=0; iter<20; iter++) {
            Tensor<double> g;
            target->value_and_gradient(x, f, g);
            gnorm = g.normf();
            print("iteration",iter,"value",f,"gradient",gnorm);
            print(x);
            if (converged()) break;
            
            if (iter > 0) {
                hessian_update_bfgs(dx, g, gp);
            }

            dx = new_search_direction(g);
            
            double step = line_search(1.0, f, dx.trace(g), x, dx);

            dx.scale(step);
            x += dx;
            gp = g;
            fp = f; 

        }
        return converged();
    }

    bool converged() const {return gnorm < tol;}

    double value() const {return f;}

    double gradient_norm() const {return gnorm;}
};


struct Test : public OptimizationTargetInterface {
    bool provides_gradient() const {return true;}

    double value(const Tensor<double>& x) {
        return 0.5*3.0*x.sumsq();
    }

    Tensor<double> gradient(const Tensor<double>& x) {
        return x*3.0;
    }
};


struct Test2 : public OptimizationTargetInterface {
    bool provides_gradient() const {return true;}

    double value(const Tensor<double>& x) {
        double v = 1.0;
        for (int i=0; i<x.dim[0]; i++) {
            v *= cos((i+1)*x[i]);
        }
        return v;
    }

    Tensor<double> gradient(const Tensor<double>& x) {
        double v = value(x);
        Tensor<double> g(x.dim[0]);
        for (int i=0; i<x.dim[0]; i++) {
            g[i]= -v*(i+1)*sin((i+1)*x[i])/cos((i+1)*x[i]);
        }
        return g;
    }
};



Tensor<double> op(const Tensor<double>& x) {
    const long n = x.dim[0];
    Tensor<double> f(n);
    for (long i=0; i<n; i++) {
        f(i) = (i + 1)*x[i]; // + 0.01*i*x[i]*x[i]*x[i];
        for (long j=0; j<n; j++) 
            f(i) += 0.0001*i*j*x[i]*x[i]*x[j]*x[j]/((i+1)*(j+1));
    }
    return f;
}

double dot_product(const Tensor<double>& a, const Tensor<double>& b) {
    return a.trace(b);
}

int main() {

    Tensor<double> x(5);
    x.fillrandom();
    BFGS solver(SharedPtr<OptimizationTargetInterface>(new Test2));
    solver.optimize(x);
    return 0;


//     int n= 40;
//     int maxiter = 100;
//     int maxnvec = 20;
//     Tensor<double> f(maxnvec,n), x(maxnvec,n), Q(maxnvec,maxnvec);

//     int m = 0;
//     x(0,_).fillrandom();
//     for (int iter=0; iter<maxiter; iter++) {
//         print("\nITERATION", iter, m);
//         f(m,_) = op(x(m,_));
//         print("x");
//         print(x(m,_));
//         print(f(m,_));

//         for (int j=0; j<=m; j++) {
//             Q(j,m) = dot_product(x(j,_),f(m,_));
//             Q(m,j) = dot_product(x(m,_),f(j,_));
//         }

//         Tensor<double> c = KAIN(Q(Slice(0,m),Slice(0,m)));
//         print("THIS IS THE C I GOT");
//         print(c);
        
//         {
//             m++;

//             Tensor<double> xnew(n);
//             for (int j=0; j<m; j++) {
//                 xnew += c(j)*(x(j,_) - f(j,_));
//             }

//             double steplen = (xnew-x(m-1,_)).normf();
//             double xnorm = xnew.normf();
//             if (steplen > 0.5) {
//                 double damp = 0.5/steplen;
//                 if (damp < 0.1) damp = 0.1;
//                 print("restrictING", steplen, xnorm, damp);
//                 xnew = damp*xnew + (1.0-damp)*x(m-1,_);
//             }
            
//             if (m == maxnvec) {
//                 for (int i=1; i<m; i++) {
//                     f(i-1,_) = f(i,_);
//                     x(i-1,_) = f(i,_);
//                 }
//                 Q(Slice(0,-2),Slice(0,-2)) = copy(Q(Slice(1,-1),Slice(1,-1)));
                
//                 m--;
//             }
//             x(m,_) = xnew;
//         }
//     }
//     return 0;

}
