# Link order is a hidden variable: it moves functions across cache lines and
# 4K pages, which can shift latency by several percent (Mytkowicz et al.,
# "Producing Wrong Data Without Doing Anything Obviously Wrong!", ASPLOS'09).
# The harness builds several link orders and treats the order as noise to be
# averaged over, so the shuffle must be deterministic per seed.

# avl_shuffle(<out_var> <seed> <items>...): seeded Fisher-Yates using a
# 31-bit LCG, because CMake has no seeded shuffle of its own.
function(avl_shuffle out_var seed)
  set(items ${ARGN})
  list(LENGTH items n)
  set(state ${seed})
  set(result "")
  while(n GREATER 0)
    math(EXPR state "(${state} * 1103515245 + 12345) % 2147483648")
    math(EXPR idx "(${state} / 65536) % ${n}")
    list(GET items ${idx} pick)
    list(REMOVE_AT items ${idx})
    list(APPEND result ${pick})
    math(EXPR n "${n} - 1")
  endwhile()
  set(${out_var} ${result} PARENT_SCOPE)
endfunction()