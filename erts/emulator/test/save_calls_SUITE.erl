%%
%% %CopyrightBegin%
%% 
%% Copyright Ericsson AB 1999-2009. All Rights Reserved.
%% 
%% The contents of this file are subject to the Erlang Public License,
%% Version 1.1, (the "License"); you may not use this file except in
%% compliance with the License. You should have received a copy of the
%% Erlang Public License along with this software. If not, it can be
%% retrieved online at http://www.erlang.org/.
%% 
%% Software distributed under the License is distributed on an "AS IS"
%% basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
%% the License for the specific language governing rights and limitations
%% under the License.
%% 
%% %CopyrightEnd%
%%

-module(save_calls_SUITE).

-include("test_server.hrl").

-export([all/1]).

-export([save_calls_1/1,dont_break_reductions/1]).

-export([do_bopp/1, do_bipp/0, do_bepp/0]).

all(suite) ->
    [save_calls_1, dont_break_reductions].

dont_break_reductions(suite) ->
    [];
dont_break_reductions(doc) ->
    ["Check that save_calls dont break reduction-based scheduling"];
dont_break_reductions(Config) when is_list(Config) ->
    ?line RPS1 = reds_per_sched(0),
    ?line RPS2 = reds_per_sched(20),
    ?line Diff = abs(RPS1 - RPS2),
    ?line true = (Diff < (0.05 * RPS1)),
    ok.


reds_per_sched(SaveCalls) ->
    ?line Parent = self(),
    ?line HowMany = 10000,
    ?line Pid = spawn(fun() -> 
			process_flag(save_calls,SaveCalls), 
			receive 
			    go -> 
				carmichaels_below(HowMany), 
				Parent ! erlang:process_info(self(),reductions)
			end 
		end),
    ?line TH = spawn(fun() -> trace_handler(0,Parent,Pid) end),
    ?line erlang:trace(Pid, true,[running,procs,{tracer,TH}]),
    ?line Pid ! go,
    ?line {Sched,Reds} = receive 
		       {accumulated,X} -> 
			   receive {reductions,Y} -> 
				   {X,Y} 
			   after 30000 -> 
				   timeout 
			   end 
		   after 30000 -> 
			   timeout 
		   end,
    ?line Reds div Sched.



trace_handler(Acc,Parent,Client) ->
    receive
	{trace,Client,out,_} ->
	    trace_handler(Acc+1,Parent,Client);
	{trace,Client,exit,_} ->
	    Parent ! {accumulated, Acc};
	_ ->
	    trace_handler(Acc,Parent,Client)
    after 10000 ->
	    ok
    end.

save_calls_1(doc) -> "Test call saving.";
save_calls_1(Config) when is_list(Config) ->
    case test_server:is_native(?MODULE) of
	true -> {skipped,"Native code"};
	false -> save_calls_1()
    end.
	    
save_calls_1() ->
    ?line erlang:process_flag(self(), save_calls, 0),
    ?line {last_calls, false} = process_info(self(), last_calls),

    ?line erlang:process_flag(self(), save_calls, 10),
    ?line {last_calls, _L1} = process_info(self(), last_calls),
    ?line ?MODULE:do_bipp(),
    ?line {last_calls, L2} = process_info(self(), last_calls),
    ?line L21 = lists:filter(fun is_local_function/1, L2),
    ?line case L21 of
	      [{?MODULE,do_bipp,0},
	       timeout,
	       'send',
	       {?MODULE,do_bopp,1},
	       'receive',
	       timeout,
	       {?MODULE,do_bepp,0}] ->
		  ok;
	      X ->
		  test_server:fail({l21, X})
	  end,

    ?line erlang:process_flag(self(), save_calls, 10),
    ?line {last_calls, L3} = process_info(self(), last_calls),
    ?line L31 = lists:filter(fun is_local_function/1, L3),
    ?line [] = L31,
    ok.

do_bipp() ->
    do_bopp(0),
    do_bapp(),
    ?MODULE:do_bopp(0),
    do_bopp(3),
    apply(?MODULE, do_bepp, []).

do_bapp() ->
    self() ! heffaklump.

do_bopp(T) ->
    receive
	X -> X
    after T -> ok
    end.

do_bepp() ->
    ok.

is_local_function({?MODULE, _, _}) ->
    true;
is_local_function({_, _, _}) ->
    false;
is_local_function(_) ->
    true.


% Number crunching for reds test.
carmichaels_below(N) ->
    random:seed(3172,9814,20125),
    carmichaels_below(1,N).

carmichaels_below(N,N2) when N >= N2 ->
    0;
carmichaels_below(N,N2) ->
    X = case fast_prime(N,10) of
	false -> 0;
	true ->
	    case fast_prime2(N,10) of
		true ->
		    %io:format("Prime: ~p~n",[N]),
		    0;
		false ->
		    io:format("Carmichael: ~p (dividable by ~p)~n",
			      [N,smallest_divisor(N)]),
		    1
	    end
    end,
    X+carmichaels_below(N+2,N2).

expmod(_,E,_) when E == 0 ->
    1;
expmod(Base,Exp,Mod) when (Exp rem 2) == 0 ->
    X = expmod(Base,Exp div 2,Mod),
    (X*X) rem Mod;
expmod(Base,Exp,Mod) -> 
    (Base * expmod(Base,Exp - 1,Mod)) rem Mod.

uniform(N) ->
    random:uniform(N-1).

fermat(N) ->    
    R = uniform(N),
    expmod(R,N,N) == R.

do_fast_prime(1,_) ->
    true;
do_fast_prime(_N,0) ->
    true;
do_fast_prime(N,Times) ->
    case fermat(N) of
	true ->
	    do_fast_prime(N,Times-1);
	false ->
	    false
    end.
    
fast_prime(N,T) ->
    do_fast_prime(N,T).

expmod2(_,E,_) when E == 0 ->
    1;
expmod2(Base,Exp,Mod) when (Exp rem 2) == 0 ->
%% Uncomment the code below to simulate scheduling bug!
%     case erlang:process_info(self(),last_calls) of
% 	{last_calls,false} -> ok;
% 	_ -> erlang:yield()
%     end,
    X = expmod2(Base,Exp div 2,Mod),
    Y=(X*X) rem Mod,
    if 
	Y == 1, X =/= 1, X =/= (Mod - 1) ->
	    0;
	true ->
	    Y rem Mod
    end;
expmod2(Base,Exp,Mod) -> 
    (Base * expmod2(Base,Exp - 1,Mod)) rem Mod.

miller_rabbin(N) ->
    R = uniform(N),
    expmod2(R,N,N) == R.

do_fast_prime2(1,_) ->
    true;
do_fast_prime2(_N,0) ->
    true;
do_fast_prime2(N,Times) ->
    case miller_rabbin(N) of
	true ->
	    do_fast_prime2(N,Times-1);
	false ->
	    false
    end.
    
fast_prime2(N,T) ->
    do_fast_prime2(N,T).

smallest_divisor(N) ->
    find_divisor(N,2).

find_divisor(N,TD) ->
    if 
	TD*TD > N ->
	    N;
	true ->
	    case divides(TD,N) of
		true ->
		    TD;
		false ->
		    find_divisor(N,TD+1)
	    end
    end.

divides(A,B) ->
    (B rem A) == 0.

