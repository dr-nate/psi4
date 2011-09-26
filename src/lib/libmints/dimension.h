#ifndef _psi_src_lib_libmints_dimension_h_
#define _psi_src_lib_libmints_dimension_h_

#include <string>
#include <cstdio>
#include <vector>

namespace psi {

extern FILE *outfile;

class Dimension
{
    std::string name_;
    int n_;
    int *blocks_;

    Dimension();
public:
    Dimension(const Dimension& other);
    Dimension(int n, const std::string& name = "");
    Dimension(const std::vector<int>& other);
    ~Dimension();

    /// Assignment operator
    Dimension& operator=(const Dimension& other);

    /// Return the dimension
    int n() const { return n_; }

    /// Return the name of the dimension.
    const std::string& name() const { return name_; }

    /// Blocks access
    int& operator[](int i) { return blocks_[i]; }
    const int& operator[](int i) const { return blocks_[i]; }

    /// Casting operator to int*
    operator int*() const { return blocks_; }
    /// Casting operator to const int*
    operator const int*() const { return blocks_; }

    /// Return the sum of constituent dimensions
    int sum() const;

    int* pointer() const { return blocks_; }

    void print(FILE* out=outfile) const;
};

}

#endif // _psi_src_lib_libmints_dimension_h_
