

#======================================================
                GLOBAL PARSING
======================================================#



starpu_parse_key_word_parsing_function = Dict{Symbol, Function}()

"""
    Translates x Expr into a new StarpuExpr object
"""
function starpu_parse(x :: Expr)

    if (x.head == :macrocall)
        if (x.args[1] != Symbol("@parallel"))
            error("Only @parallel macro, used before a for loop, is allowed ($(x.args[1]) was found)")
        end

        if (length(x.args) != 3)
            error("Invalid usage of @parallel macro", length(x.args))
        end
        return starpu_parse_for(x.args[3], is_independant = true)
    end

    if !(x.head in keys(starpu_parse_key_word_parsing_function))
        return StarpuExprInvalid() #TODO error ?
    end

    return starpu_parse_key_word_parsing_function[x.head](x)

end

for kw in (:if, :call, :for, :block, :return, :function, :while, :ref, :break)
    starpu_parse_key_word_parsing_function[kw] = eval(Symbol(:starpu_parse_, kw))
end

starpu_parse_key_word_parsing_function[:(:)] = starpu_parse_interval
starpu_parse_key_word_parsing_function[:(::)] = starpu_parse_typed
starpu_parse_key_word_parsing_function[:(=)] = starpu_parse_affect
starpu_parse_key_word_parsing_function[:(.)] = starpu_parse_field


"""
    Executes the starpu_parse function on the following expression,
    and returns the obtained StarpuExpr
"""
macro parse(x)
    y = Expr(:quote, x)
    :(starpu_parse($y))
end
