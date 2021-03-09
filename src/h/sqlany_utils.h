// ***************************************************************************
// Copyright (c) 2018 SAP SE or an SAP affiliate company. All rights reserved.
// ***************************************************************************
#include <iostream>
#include <string>
#include <string.h>
#include <sstream> 
#include <vector>
#include "sacapidll.h"
#include "sacapi.h"

#pragma warning (disable : 4100)
#pragma warning (disable : 4506)
#pragma warning (disable : 4068)
#pragma warning (disable : 4800)
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include "node.h"
#include "v8.h"
#include "node_buffer.h"
#include "node_object_wrap.h"
#include "uv.h"

#pragma GCC diagnostic pop
#pragma warning (default : 4100)
#pragma warning (default : 4506)
#pragma warning (default : 4068)
#pragma warning (default : 4800)

#include "nodever_cover.h"
#include "errors.h"
#include "connection.h"
#include "stmt.h"

using namespace v8;

extern SQLAnywhereInterface api;
extern unsigned openConnections;
extern uv_mutex_t api_mutex;

#define CLEAN_STRINGS( vector )      		\
{                                          	\
    for( size_t i = 0; i < vector.size(); i++ ) { 	\
	delete[] vector[i];			\
    }						\
    vector.clear();				\
}

#define CLEAN_PTRS( vector )      		\
{                                          	\
    for( size_t i = 0; i < vector.size(); i++ ) { 	\
	delete vector[i];			\
    }						\
    vector.clear();				\
}

class scoped_lock
{
    public:
	scoped_lock( uv_mutex_t &mtx ) : _mtx( mtx )
	{
	    uv_mutex_lock( &_mtx );
	}
	~scoped_lock()
	{
	    
	    uv_mutex_unlock( &_mtx );
	}

    private:
	uv_mutex_t &_mtx;

};

class ExecuteData {
  public:
    ~ExecuteData() {
	clear();
    }

    void clear( void ) {
	CLEAN_STRINGS( string_vals );
	CLEAN_STRINGS( string_arr_vals );
	CLEAN_PTRS( int_vals );
	CLEAN_PTRS( num_vals );
	CLEAN_PTRS( len_vals );
	CLEAN_PTRS( null_vals );
    }
    void	addString( char *str, size_t *len ) {
	string_vals.push_back( str );
	len_vals.push_back( len );
    }
    void	addStrings( char **str, size_t *len ) {
	string_arr_vals.push_back( str );
	len_vals.push_back( len );
    }
    void	addInt( int *val ) { int_vals.push_back( val ); }
    void	addNum( double *val ) { num_vals.push_back( val ); }
    void	addNull( sacapi_bool *val ) { null_vals.push_back( val ); }

    char *	getString( size_t ind ) { return string_vals[ind]; }
    char **	getStrings( size_t ind ) { return string_arr_vals[ind]; }
    int		getInt( size_t ind ) { return *(int_vals[ind]); }
    double	getNum( size_t ind ) { return *(num_vals[ind]); }
    size_t	getLen( size_t ind ) { return *(len_vals[ind]); }
    sacapi_bool	getNull( size_t ind ) { return *(null_vals[ind]); }

    size_t	stringSize( void ) const { return string_vals.size(); }
    size_t	intSize( void ) const { return int_vals.size(); }
    size_t	numSize( void ) const { return num_vals.size(); }
    size_t	lenSize( void ) const { return len_vals.size(); }
    size_t	nullSize( void ) const { return null_vals.size(); }

    bool	stringIsNull( size_t ind ) const { return string_vals[ind] == NULL; }
    bool	intIsNull( size_t ind ) const { return int_vals[ind] == NULL; }
    bool	numIsNull( size_t ind ) const { return num_vals[ind] == NULL; }
    bool	lenIsNull( size_t ind ) const { return string_vals[ind] == NULL; }
    bool	nullIsNull( size_t ind ) const { return string_vals[ind] == NULL; }

  private:
    std::vector<char *>		string_vals;
    std::vector<char **>	string_arr_vals;
    std::vector<int *> 		int_vals;
    std::vector<double *>	num_vals;
    std::vector<size_t *>	len_vals;
    std::vector<sacapi_bool *>	null_vals;
};

bool cleanAPI (); // Finalizes the API and frees up resources
void getErrorMsg( a_sqlany_connection *conn, std::string &str );
void getErrorMsg( int code, std::string &str );
void throwError( a_sqlany_connection *conn );   
void throwError( int code );

#if v010
void callBack( std::string *		str, 
	       Persistent<Function>	callback,
	       Local<Value>		Result,
	       bool			callback_required = true );
void callBack( std::string *		str,
	       Local<Value>		callback,
	       Local<Value>		Result,
	       bool			callback_required = true );
#else
void callBack( std::string *		str,
	       Persistent<Function> &	callback,
	       Local<Value> &		Result,
	       bool			callback_required = true );
void callBack( std::string *		str,
	       const Local<Value> &	callback,
	       Local<Value> &		Result,
	       bool			callback_required = true );
void callBack( std::string *		str,
	       Persistent<Function> &	callback,
	       Persistent<Value> &	Result,
	       bool			callback_required = true );
#endif

bool getBindParameters( std::vector<ExecuteData *>		&execData
            , Isolate *                 isolate
			, Local<Value>				arg
			, std::vector<a_sqlany_bind_param> 	&params
			, unsigned				&num_rows
    );

#if v010
bool getResultSet( Local<Value> 			&Result   
		 , int 					&rows_affected
		 , std::vector<char *> 			&colNames
		 , ExecuteData				*execData
		 , std::vector<a_sqlany_data_type> 	&col_types );
#else
bool getResultSet( Persistent<Value> 			&Result   
		 , int 					&rows_affected
		 , std::vector<char *> 			&colNames
		 , ExecuteData				*execData
		 , std::vector<a_sqlany_data_type> 	&col_types );
#endif

bool fetchResultSet( a_sqlany_stmt 			*sqlany_stmt
		   , int 				&rows_affected
		   , std::vector<char *> 		&colNames
		   , ExecuteData			*execData
		   , std::vector<a_sqlany_data_type> 	&col_types );

struct noParamBaton {
    Persistent<Function> 	callback;
    bool 			err;
    std::string 		error_msg;
    bool 			callback_required;
    
    Connection *obj;
    
    noParamBaton() {
	obj = NULL;
	err = false;
    }
    
    ~noParamBaton() {
	obj = NULL;
    }
};

void executeAfter( uv_work_t *req );
void executeWork( uv_work_t *req );

#if NODE_MAJOR_VERSION > 0 || NODE_MINOR_VERSION > 10
void HashToString( Isolate *isolate, Local<Object> obj, Persistent<String> &ret );
#else
Persistent<String> HashToString( Local<Object> obj );
Handle<Value> CreateConnection( const Arguments &args );
#endif
