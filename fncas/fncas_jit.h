// https://github.com/dkorolev/fncas

// FNCAS on-the-fly compilation logic.
// FNCAS_JIT must be defined to enable, supported values are 'NASM' and 'CLANG'.

#ifndef FNCAS_JIT_H
#define FNCAS_JIT_H

#ifdef FNCAS_JIT

#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include <dlfcn.h>

#include <boost/format.hpp>

#include "fncas_base.h"
#include "fncas_node.h"

namespace fncas {

// Linux-friendly code to compile into .so and link against it at runtime.
// Not portable.

struct compiled_expression : noncopyable {
  typedef long long (*DIM)();
  typedef double (*EVAL)(const double* x, double* a);
  void* lib_;
  DIM dim_;
  EVAL eval_;
  const std::string lib_filename_;
  explicit compiled_expression(const std::string& lib_filename) : lib_filename_(lib_filename) {
    lib_ = dlopen(lib_filename.c_str(), RTLD_LAZY);
    assert(lib_);
    dim_ = reinterpret_cast<DIM>(dlsym(lib_, "dim"));
    eval_ = reinterpret_cast<EVAL>(dlsym(lib_, "eval"));
    assert(dim_);
    assert(eval_);
  }
  ~compiled_expression() {
    if (lib_) {
      dlclose(lib_);
    }
  }
  compiled_expression(const compiled_expression&) = delete;
  void operator=(const compiled_expression&) = delete;
  compiled_expression(compiled_expression&& rhs)
      : lib_(std::move(rhs.lib_)),
        dim_(std::move(rhs.dim_)),
        eval_(std::move(rhs.eval_)),
        lib_filename_(std::move(rhs.lib_filename_)) {
    rhs.lib_ = nullptr;
  }
  double operator()(const double* x) const {
    std::vector<double>& tmp = internals_singleton().ram_for_compiled_evaluations_;
    node_index_type dim = static_cast<node_index_type>(dim_());
    if (tmp.size() < dim) {
      tmp.resize(dim);
    }
    return eval_(x, &tmp[0]);
  }
  double operator()(const std::vector<double>& x) const {
    return operator()(&x[0]);
  }
  node_index_type dim() const {
    return dim_ ? static_cast<node_index_type>(dim_()) : 0;
  }
  static void syscall(const std::string& command) {
    int retval = system(command.c_str());
    if (retval) {
      std::cerr << command << std::endl << retval << std::endl;
      exit(-1);
    }
  }
  const std::string& lib_filename() const {
    return lib_filename_;
  }
};

// generate_c_code_for_node() writes C code to evaluate the expression to the file.
void generate_c_code_for_node(node_index_type index, FILE* f) {
  fprintf(f, "#include <math.h>\n");
  fprintf(f, "double eval(const double* x, double* a) {\n");
  node_index_type max_dim = index;
  std::stack<node_index_type> stack;
  stack.push(index);
  while (!stack.empty()) {
    const node_index_type i = stack.top();
    stack.pop();
    const node_index_type dependent_i = ~i;
    if (i > dependent_i) {
      max_dim = std::max(max_dim, static_cast<node_index_type>(i));
      node_impl& node = node_vector_singleton()[i];
      if (node.type() == type_t::variable) {
        int32_t v = node.variable();
        fprintf(f, "  a[%lld] = x[%d];\n", static_cast<long long>(i), v);
      } else if (node.type() == type_t::value) {
        fprintf(
            f, "  a[%lld] = %a;\n", static_cast<long long>(i), node.value());  // "%a" is hexadecimal full precision.
      } else if (node.type() == type_t::operation) {
        stack.push(~i);
        stack.push(node.lhs_index());
        stack.push(node.rhs_index());
      } else if (node.type() == type_t::function) {
        stack.push(~i);
        stack.push(node.argument_index());
      } else {
        assert(false);
      }
    } else {
      node_impl& node = node_vector_singleton()[dependent_i];
      if (node.type() == type_t::operation) {
        fprintf(f,
                "  a[%lld] = a[%lld] %s a[%lld];\n",
                static_cast<long long>(dependent_i),
                static_cast<long long>(node.lhs_index()),
                operation_as_string(node.operation()),
                static_cast<long long>(node.rhs_index()));
      } else if (node.type() == type_t::function) {
        fprintf(f,
                "  a[%lld] = %s(a[%lld]);\n",
                static_cast<long long>(dependent_i),
                function_as_string(node.function()),
                static_cast<long long>(node.argument_index()));
      } else {
        assert(false);
      }
    }
  }
  fprintf(f, "  return a[%lld];\n", static_cast<long long>(index));
  fprintf(f, "}\n");
  fprintf(f, "long long dim() { return %lld; }\n", static_cast<long long>(max_dim + 1));
}

// generate_asm_code_for_node() writes NASM code to evaluate the expression to the file.
const char* const operation_as_nasm_instruction(operation_t operation) {
  static const char* representation[static_cast<size_t>(operation_t::end)] = {
      "addpd", "subpd", "mulpd", "divpd",
  };
  return operation < operation_t::end ? representation[static_cast<size_t>(operation)] : "?";
}
void generate_asm_code_for_node(node_index_type index, FILE* f) {
  fprintf(f, "[bits 64]\n");
  fprintf(f, "\n");
  fprintf(f, "global eval, dim\n");
  fprintf(f, "extern sqrt, exp, log, sin, cos, tan, asin, acos, atan\n");
  fprintf(f, "\n");
  fprintf(f, "section .text\n");
  fprintf(f, "\n");
  fprintf(f, "eval:\n");
  fprintf(f, "  push rbp\n");
  fprintf(f, "  mov rbp, rsp\n");
  node_index_type max_dim = index;
  std::stack<node_index_type> stack;
  stack.push(index);
  while (!stack.empty()) {
    const node_index_type i = stack.top();
    stack.pop();
    const node_index_type dependent_i = ~i;
    if (i > dependent_i) {
      max_dim = std::max(max_dim, static_cast<node_index_type>(i));
      node_impl& node = node_vector_singleton()[i];
      if (node.type() == type_t::variable) {
        int32_t v = node.variable();
        fprintf(f, "  ; a[%lld] = x[%d];\n", static_cast<long long>(i), v);
        fprintf(f, "  mov rax, [rdi+%d]\n", v * 8);
        fprintf(f, "  mov [rsi+%lld], rax\n", static_cast<long long>(i) * 8);
      } else if (node.type() == type_t::value) {
        fprintf(f,
                "  ; a[%lld] = %a;\n",
                static_cast<long long>(i),
                node.value());  // "%a" is hexadecimal full precision.
        fprintf(f, "  mov rax, %s\n", std::to_string(*reinterpret_cast<int64_t*>(&node.value())).c_str());
        fprintf(f, "  mov [rsi+%lld], rax\n", static_cast<long long>(i) * 8);
      } else if (node.type() == type_t::operation) {
        stack.push(~i);
        stack.push(node.lhs_index());
        stack.push(node.rhs_index());
      } else if (node.type() == type_t::function) {
        stack.push(~i);
        stack.push(node.argument_index());
      } else {
        assert(false);
      }
    } else {
      node_impl& node = node_vector_singleton()[dependent_i];
      if (node.type() == type_t::operation) {
        fprintf(f,
                "  ; a[%lld] = a[%lld] %s a[%lld];\n",
                static_cast<long long>(dependent_i),
                static_cast<long long>(node.lhs_index()),
                operation_as_string(node.operation()),
                static_cast<long long>(node.rhs_index()));
        fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(node.lhs_index()) * 8);
        fprintf(f, "  movq xmm1, [rsi+%lld]\n", static_cast<long long>(node.rhs_index()) * 8);
        fprintf(f, "  %s xmm0, xmm1\n", operation_as_nasm_instruction(node.operation()));
        fprintf(f, "  movq [rsi+%lld], xmm0\n", static_cast<long long>(dependent_i) * 8);
      } else if (node.type() == type_t::function) {
        fprintf(f,
                "  ; a[%lld] = %s(a[%lld]);\n",
                static_cast<long long>(dependent_i),
                function_as_string(node.function()),
                static_cast<long long>(node.argument_index()));
        fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(node.argument_index()) * 8);
        fprintf(f, "  push rdi\n");
        fprintf(f, "  push rsi\n");
        fprintf(f, "  call %s wrt ..plt\n", function_as_string(node.function()));
        fprintf(f, "  pop rsi\n");
        fprintf(f, "  pop rdi\n");
        fprintf(f, "  movq [rsi+%lld], xmm0\n", static_cast<long long>(dependent_i) * 8);
      } else {
        assert(false);
      }
    }
  }
  fprintf(f, "  ; return a[%lld]\n", static_cast<long long>(index));
  fprintf(f, "  movq xmm0, [rsi+%lld]\n", static_cast<long long>(index) * 8);
  fprintf(f, "  mov rsp, rbp\n");
  fprintf(f, "  pop rbp\n");
  fprintf(f, "  ret\n");
  fprintf(f, "\n");
  fprintf(f, "dim:\n");
  fprintf(f, "  push rbp\n");
  fprintf(f, "  mov rbp, rsp\n");
  fprintf(f, "  mov rax, %lld\n", static_cast<long long>(max_dim + 1));
  fprintf(f, "  mov rsp, rbp\n");
  fprintf(f, "  pop rbp\n");
  fprintf(f, "  ret\n");
}

struct compile_impl {
  struct NASM {
    static void compile(const std::string& filebase, node_index_type index) {
      FILE* f = fopen((filebase + ".asm").c_str(), "w");
      assert(f);
      generate_asm_code_for_node(index, f);
      fclose(f);

      const char* compile_cmdline = "nasm -f elf64 %1%.asm -o %1%.o";
      const char* link_cmdline = "ld -lm -shared -o %1%.so %1%.o";

      compiled_expression::syscall((boost::format(compile_cmdline) % filebase).str());
      compiled_expression::syscall((boost::format(link_cmdline) % filebase).str());
    }
  };
  struct CLANG {
    static void compile(const std::string& filebase, node_index_type index) {
      FILE* f = fopen((filebase + ".c").c_str(), "w");
      assert(f);
      generate_c_code_for_node(index, f);
      fclose(f);

      const char* compile_cmdline = "clang -fPIC -shared -nostartfiles %1%.c -o %1%.so";
      std::string cmdline = (boost::format(compile_cmdline) % filebase).str();
      compiled_expression::syscall(cmdline);
    }
  };
  // Confirm FNCAS_JIT is a valid identifier.
  struct _TMP {
    struct FNCAS_JIT {};
  };
  typedef FNCAS_JIT selected;
};

compiled_expression compile(node_index_type index) {
  std::random_device random;
  std::uniform_int_distribution<int> distribution(1000000, 9999999);
  std::ostringstream os;
  os << "/tmp/" << distribution(random);
  const std::string filebase = os.str();
  const std::string filename_so = filebase + ".so";
  unlink(filename_so.c_str());
  compile_impl::selected::compile(filebase, index);
  return compiled_expression(filename_so);
}

compiled_expression compile(const node& node) {
  return compile(node.index_);
}

struct f_compiled : f {
  fncas::compiled_expression c_;
  explicit f_compiled(const node& node) : c_(compile(node)) {
  }
  explicit f_compiled(const f_intermediate& f) : c_(compile(f.f_)) {
  }
  f_compiled(const f_compiled&) = delete;
  void operator=(const f_compiled&) = delete;
  f_compiled(f_compiled&& rhs) : c_(std::move(rhs.c_)) {
  }
  virtual double operator()(const std::vector<double>& x) const {
    return c_(x);
  }
  virtual int32_t dim() const {
    return c_.dim();
  }
  const std::string& lib_filename() const {
    return c_.lib_filename();
  }
};

}  // namespace fncas

#endif  // #ifdef FNCAS_JIT

#endif  // #ifndef FNCAS_JIT_H
