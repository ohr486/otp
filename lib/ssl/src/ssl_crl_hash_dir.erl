%%
%% %CopyrightBegin%
%%
%% SPDX-License-Identifier: Apache-2.0
%%
%% Copyright Ericsson AB 2016-2025. All Rights Reserved.
%%
%% Licensed under the Apache License, Version 2.0 (the "License");
%% you may not use this file except in compliance with the License.
%% You may obtain a copy of the License at
%%
%%     http://www.apache.org/licenses/LICENSE-2.0
%%
%% Unless required by applicable law or agreed to in writing, software
%% distributed under the License is distributed on an "AS IS" BASIS,
%% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
%% See the License for the specific language governing permissions and
%% limitations under the License.
%%
%% %CopyrightEnd%

-module(ssl_crl_hash_dir).
-moduledoc false.

-include_lib("public_key/include/public_key.hrl").
-include_lib("kernel/include/logger.hrl").

-behaviour(ssl_crl_cache_api).

-export([lookup/3, select/2, fresh_crl/2]).

lookup(#'DistributionPoint'{cRLIssuer = CRLIssuer} = DP, CertIssuer, CRLDbInfo) ->
    case CRLIssuer of
        asn1_NOVALUE ->
            %% If the distribution point extension doesn't
            %% indicate a CRL issuer, use the certificate issuer.
            select(CertIssuer, CRLDbInfo);
        _ ->
            CRLs = select(CRLIssuer, CRLDbInfo),
            lists:filter(fun(DER) ->
                                 public_key:pkix_match_dist_point(DER, DP)
                         end, CRLs)
    end.

fresh_crl(#'DistributionPoint'{}, CurrentCRL) ->
    CurrentCRL.

select({rdnSequence, _} = Issuer, DbHandle) ->
  select([{directoryName, Issuer}], DbHandle);
select([], _) ->
    [];
select([{directoryName, Issuer} | _], {_DbHandle, [{dir, Dir}]}) ->
    case find_crls(public_key:pkix_normalize_name(Issuer), Dir) of
        {#{reason := []}, DERs} ->
            DERs;
        {#{reason := [_|_]} = Report, DERs} ->
            {logger, {notice, Report, ?LOCATION}, DERs};
        {error, Error} ->
            {logger, {error, #{description => "CRL retrieval",
                               reason => [{cannot_find_crl, Error},
                                          {dir, Dir}]}, ?LOCATION}, []}
    end;
select([_ | Rest], CRLDbInfo) ->
    select(Rest, CRLDbInfo).

find_crls(Issuer, Dir) ->
    case filelib:is_dir(Dir) of
        true ->
	    Hash = public_key:short_name_hash(Issuer),
	    find_crls(Issuer, Hash, Dir, 0, [], #{description => "CRL file traversal",
                                                  reason => []});
        false ->
            {error, not_a_directory}
    end.

find_crls(Issuer, Hash, Dir, N, Acc, #{reason := Reason} = Report) ->
    Filename = filename:join(Dir, Hash ++ ".r" ++ integer_to_list(N)),
    case file:read_file(Filename) of
	{error, enoent} ->
            {Report, Acc};
        {ok, Bin} ->
	    try maybe_parse_pem(Bin) of
		DER when is_binary(DER) ->
		    %% Found one file.  Let's see if there are more.
		    find_crls(Issuer, Hash, Dir, N + 1, [DER] ++ Acc, Report)
	    catch
		error:Error ->
		    %% Something is wrong with the file.  Report
		    %% it, and try the next one.
		    find_crls(Issuer, Hash, Dir, N + 1, Acc, Report#{reason => [{{crl_parse_error, Error},
                                                                                 {filename, Filename}} | Reason]})
	    end
    end.

maybe_parse_pem(<<"-----BEGIN", _/binary>> = PEM) ->
    %% It's a PEM encoded file.  Need to extract the DER
    %% encoded data.
    [{'CertificateList', DER, not_encrypted}] = public_key:pem_decode(PEM),
    DER;
maybe_parse_pem(DER) when is_binary(DER) ->
    %% Let's assume it's DER-encoded.
    DER.

