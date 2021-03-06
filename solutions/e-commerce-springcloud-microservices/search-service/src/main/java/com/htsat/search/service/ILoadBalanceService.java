package com.htsat.search.service;
import com.htsat.search.serviceimpl.LoadBalanceServiceImpl;
import org.springframework.cloud.netflix.feign.FeignClient;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestMethod;
import org.springframework.web.bind.annotation.RequestParam;

@FeignClient(value = "service-hi", fallback = LoadBalanceServiceImpl.class)
public interface ILoadBalanceService {
    @RequestMapping(value = "/hi",method = RequestMethod.GET)
    String loadbalanceService(@RequestParam(value = "name") String name);
}
